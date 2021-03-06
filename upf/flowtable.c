/*
 * Copyright (c) 2016 Qosmos and/or its affiliates.
 * Copyright (c) 2018 Travelping GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vppinfra/dlist.h>
#include <vppinfra/types.h>
#include <vppinfra/vec.h>
#include <vnet/ip/ip4_packet.h>

#include "upf.h"
#include "upf_app_db.h"
#include "flowtable.h"
#include "flowtable_tcp.h"

#if CLIB_DEBUG > 1
#define upf_debug clib_warning
#else
#define upf_debug(...)				\
  do { } while (0)
#endif

vlib_node_registration_t upf_flow_node;

always_inline void
flow_entry_cache_fill (flowtable_main_t * fm, flowtable_main_per_cpu_t * fmt)
{
#if CLIB_DEBUG > 0
  u32 cpu_index = os_get_thread_index ();
#endif
  int i;
  flow_entry_t *f;

  if (pthread_spin_lock (&fm->flows_lock) == 0)
    {
      if (PREDICT_FALSE (fm->flows_cpt > fm->flows_max))
	{
	  pthread_spin_unlock (&fm->flows_lock);
	  return;
	}

      for (i = 0; i < FLOW_CACHE_SZ; i++)
	{
	  pool_get_aligned (fm->flows, f, CLIB_CACHE_LINE_BYTES);
#if CLIB_DEBUG > 0
	  f->cpu_index = cpu_index;
#endif
	  vec_add1 (fmt->flow_cache, f - fm->flows);
	}
      fm->flows_cpt += FLOW_CACHE_SZ;

      pthread_spin_unlock (&fm->flows_lock);
    }
}

always_inline void
flow_entry_cache_empty (flowtable_main_t * fm, flowtable_main_per_cpu_t * fmt)
{
#if CLIB_DEBUG > 0
  u32 cpu_index = os_get_thread_index ();
#endif
  int i;

  if (pthread_spin_lock (&fm->flows_lock) == 0)
    {
      for (i = vec_len (fmt->flow_cache) - 1; i > FLOW_CACHE_SZ; i--)
	{
	  u32 f_index = vec_pop (fmt->flow_cache);

	  upf_debug ("releasing flow %p, index %u",
		     pool_elt_at_index (fm->flows, f_index), f_index);
#if CLIB_DEBUG > 0
	  ASSERT (pool_elt_at_index (fm->flows, f_index)->cpu_index ==
		  cpu_index);
#endif

	  pool_put_index (fm->flows, f_index);
	}
      fm->flows_cpt -= FLOW_CACHE_SZ;

      pthread_spin_unlock (&fm->flows_lock);
    }
}

always_inline flow_entry_t *
flow_entry_alloc (flowtable_main_t * fm, flowtable_main_per_cpu_t * fmt)
{
  u32 f_index;
  flow_entry_t *f;

  if (vec_len (fmt->flow_cache) == 0)
    flow_entry_cache_fill (fm, fmt);

  if (PREDICT_FALSE ((vec_len (fmt->flow_cache) == 0)))
    return NULL;

  f_index = vec_pop (fmt->flow_cache);
  f = pool_elt_at_index (fm->flows, f_index);
#if CLIB_DEBUG > 0
  ASSERT (f->cpu_index == os_get_thread_index ());
#endif

  return f;
}

always_inline void
flow_entry_free (flowtable_main_t * fm, flowtable_main_per_cpu_t * fmt,
		 flow_entry_t * f)
{
#if CLIB_DEBUG > 0
  ASSERT (f->cpu_index == os_get_thread_index ());
#endif

  if (f->app_uri)
    vec_free (f->app_uri);

  vec_add1 (fmt->flow_cache, f - fm->flows);

  if (vec_len (fmt->flow_cache) > 2 * FLOW_CACHE_SZ)
    flow_entry_cache_empty (fm, fmt);
}

always_inline void
flowtable_entry_remove (flowtable_main_per_cpu_t * fmt, flow_entry_t * f)
{
  BVT (clib_bihash_kv) kv;

  clib_memcpy (kv.key, f->key.key, sizeof (kv.key));
  BV (clib_bihash_add_del) (&fmt->flows_ht, &kv, 0 /* is_add */ );
}

always_inline void
expire_single_flow (flowtable_main_t * fm, flowtable_main_per_cpu_t * fmt,
		    flow_entry_t * f, dlist_elt_t * e, u32 now)
{
  ASSERT (f->timer_index == (e - fmt->timers));

  /* timers unlink */
  clib_dlist_remove (fmt->timers, e - fmt->timers);

  upf_debug ("Flow Timeout Check %p: %u (%u) > %u (%u)",
	     f, f->active + f->lifetime,
	     (f->active + f->lifetime) % fm->timer_max_lifetime,
	     now, fmt->time_index);

  if (f->active + f->lifetime > now)
    {
      /* There was activity on the entry, so the idle timeout
         has not passed. Enqueue for another time period. */
      u32 timer_slot_head_index;

      timer_slot_head_index =
	(f->active + f->lifetime) % fm->timer_max_lifetime;
      upf_debug ("Flow Reshedule %p to %u", f, timer_slot_head_index);
      clib_dlist_addtail (fmt->timers, timer_slot_head_index, f->timer_index);
    }
  else
    {
      upf_main_t *gtm = &upf_main;
      upf_debug ("Flow Remove %p", f);
      pool_put (fmt->timers, e);

      /* hashtable unlink */
      flowtable_entry_remove (fmt, f);

      vlib_decrement_simple_counter (&gtm->upf_simple_counters
				     [UPF_FLOW_COUNTER],
				     vlib_get_thread_index (), 0, 1);

      /* free to flow cache && pool (last) */
      flow_entry_free (fm, fmt, f);
    }
}

u64
flowtable_timer_expire (flowtable_main_t * fm, flowtable_main_per_cpu_t * fmt,
			u32 now)
{
  u32 time_slot_curr_index = fmt->time_index;
  u64 expire_cpt;
  flow_entry_t *f;
  dlist_elt_t *time_slot_curr;
  u32 index;

  if (PREDICT_FALSE (dlist_is_empty (fmt->timers, time_slot_curr_index)))
    return 0;

  expire_cpt = 0;
  time_slot_curr = pool_elt_at_index (fmt->timers, time_slot_curr_index);

  index = time_slot_curr->next;
  while (index != time_slot_curr_index && expire_cpt < TIMER_MAX_EXPIRE)
    {
      dlist_elt_t *e = pool_elt_at_index (fmt->timers, index);
      f = pool_elt_at_index (fm->flows, e->value);

      index = e->next;
      expire_single_flow (fm, fmt, f, e, now);
      expire_cpt++;
    }

  return expire_cpt;
}

static inline u16
flowtable_lifetime_calculate (flowtable_main_t * fm, flow_key_t const *key)
{
  switch (key->proto)
    {
    case IP_PROTOCOL_ICMP:
      return fm->timer_lifetime[FT_TIMEOUT_TYPE_ICMP];

    case IP_PROTOCOL_UDP:
      return fm->timer_lifetime[FT_TIMEOUT_TYPE_UDP];

    case IP_PROTOCOL_TCP:
      return fm->timer_lifetime[FT_TIMEOUT_TYPE_TCP];

    default:
      return ip46_address_is_ip4 (&key->ip[FT_ORIGIN]) ?
	fm->timer_lifetime[FT_TIMEOUT_TYPE_IPV4] :
	fm->timer_lifetime[FT_TIMEOUT_TYPE_IPV6];
    }

  return fm->timer_lifetime[FT_TIMEOUT_TYPE_UNKNOWN];
}

static void
recycle_flow (flowtable_main_t * fm, flowtable_main_per_cpu_t * fmt, u32 now)
{
  u32 next;

  next = (now + 1) % fm->timer_max_lifetime;
  while (PREDICT_FALSE (next != now))
    {
      flow_entry_t *f;
      u32 slot_index = next;

      if (PREDICT_FALSE (dlist_is_empty (fmt->timers, slot_index)))
	{
	  next = (next + 1) % fm->timer_max_lifetime;
	  continue;
	}
      dlist_elt_t *head = pool_elt_at_index (fmt->timers, slot_index);
      dlist_elt_t *e = pool_elt_at_index (fmt->timers, head->next);

      f = pool_elt_at_index (fm->flows, e->value);
      return expire_single_flow (fm, fmt, f, e, now);
    }

  /*
   * unreachable:
   * this should be called if there is no free flows, so we're bound to have
   * at least *one* flow within the timer wheel (cpu cache is filled at init).
   */
  clib_error ("recycle_flow did not find any flow to recycle !");
}

/* TODO: replace with a more appropriate hashtable */
u32
flowtable_entry_lookup_create (flowtable_main_t * fm,
			       flowtable_main_per_cpu_t * fmt,
			       BVT (clib_bihash_kv) * kv, u32 const now,
			       u8 is_reverse, u16 generation, int *created)
{
  flow_entry_t *f;
  dlist_elt_t *timer_entry;
  upf_main_t *gtm = &upf_main;

  if (PREDICT_FALSE
      (BV (clib_bihash_search_inline) (&fmt->flows_ht, kv) == 0))
    {
      return kv->value;
    }

  /* create new flow */
  f = flow_entry_alloc (fm, fmt);
  if (PREDICT_FALSE (f == NULL))
    {
      recycle_flow (fm, fmt, now);
      f = flow_entry_alloc (fm, fmt);
      if (PREDICT_FALSE (f == NULL))
	{
	  clib_error ("flowtable failed to recycle a flow");

	  vlib_node_increment_counter (fm->vlib_main, upf_flow_node.index,
				       FLOWTABLE_ERROR_RECYCLE, 1);
	  return ~0;
	}
    }

  *created = 1;

  memset (f, 0, sizeof (*f));
  clib_memcpy (f->key.key, kv->key, sizeof (f->key.key));
  f->is_reverse = is_reverse;
  f->lifetime = flowtable_lifetime_calculate (fm, &f->key);
  f->active = now;
  f->application_id = ~0;
  f->generation = generation;
  flow_pdr_id (f, FT_ORIGIN) = ~0;
  flow_pdr_id (f, FT_REVERSE) = ~0;
  flow_teid (f, FT_ORIGIN) = ~0;
  flow_teid (f, FT_REVERSE) = ~0;
  flow_next (f, FT_ORIGIN) = FT_NEXT_CLASSIFY;
  flow_next (f, FT_REVERSE) = FT_NEXT_CLASSIFY;
  flow_tc (f, FT_ORIGIN).conn_index = ~0;
  flow_tc (f, FT_ORIGIN).thread_index = ~0;
  flow_tc (f, FT_REVERSE).conn_index = ~0;
  flow_tc (f, FT_REVERSE).thread_index = ~0;

  /* insert in timer list */
  pool_get (fmt->timers, timer_entry);
  timer_entry->value = f - fm->flows;	/* index within the flow pool */
  f->timer_index = timer_entry - fmt->timers;	/* index within the timer pool */
  timer_wheel_insert_flow (fm, fmt, f);

  vlib_increment_simple_counter (&gtm->upf_simple_counters[UPF_FLOW_COUNTER],
				 vlib_get_thread_index (), 0, 1);

  /* insert in hash */
  kv->value = f - fm->flows;
  BV (clib_bihash_add_del) (&fmt->flows_ht, kv, 1 /* is_add */ );

  return kv->value;
}

void
timer_wheel_index_update (flowtable_main_t * fm,
			  flowtable_main_per_cpu_t * fmt, u32 now)
{
  u32 new_index = now % fm->timer_max_lifetime;

  if (PREDICT_FALSE (fmt->time_index == ~0))
    {
      fmt->time_index = new_index;
      return;
    }

  if (new_index != fmt->time_index)
    {
      /* reschedule all remaining flows on current time index
       * at the begining of the next one */

      u32 curr_slot_index = fmt->time_index;
      dlist_elt_t *curr_head =
	pool_elt_at_index (fmt->timers, curr_slot_index);

      u32 next_slot_index = new_index;
      dlist_elt_t *next_head =
	pool_elt_at_index (fmt->timers, next_slot_index);

      if (PREDICT_FALSE (dlist_is_empty (fmt->timers, curr_slot_index)))
	{
	  fmt->time_index = new_index;
	  return;
	}

      dlist_elt_t *curr_prev =
	pool_elt_at_index (fmt->timers, curr_head->prev);
      dlist_elt_t *curr_next =
	pool_elt_at_index (fmt->timers, curr_head->next);

      /* insert timer list of current time slot at the begining of the next slot */
      if (PREDICT_FALSE (dlist_is_empty (fmt->timers, next_slot_index)))
	{
	  next_head->next = curr_head->next;
	  next_head->prev = curr_head->prev;
	  curr_prev->next = next_slot_index;
	  curr_next->prev = next_slot_index;
	}
      else
	{
	  dlist_elt_t *next_next =
	    pool_elt_at_index (fmt->timers, next_head->next);
	  curr_prev->next = next_head->next;
	  next_head->next = curr_head->next;
	  next_next->prev = curr_head->prev;
	  curr_next->prev = next_slot_index;
	}

      /* reset current time slot as an empty list */
      memset (curr_head, 0xff, sizeof (*curr_head));

      fmt->time_index = new_index;
    }
}

u8 *
format_flow_key (u8 * s, va_list * args)
{
  flow_key_t *key = va_arg (*args, flow_key_t *);

  return format (s, "proto 0x%x, %U:%u <-> %U:%u, seid 0x%016llx",
		 key->proto,
		 format_ip46_address, &key->ip[FT_ORIGIN], IP46_TYPE_ANY,
		 clib_net_to_host_u16 (key->port[FT_ORIGIN]),
		 format_ip46_address, &key->ip[FT_REVERSE], IP46_TYPE_ANY,
		 clib_net_to_host_u16 (key->port[FT_REVERSE]), key->seid);
}

u8 *
format_flow (u8 * s, va_list * args)
{
  flow_entry_t *flow = va_arg (*args, flow_entry_t *);
  int is_reverse = flow->is_reverse;
  upf_main_t *sm = &upf_main;
  u8 *app_name = NULL;

  if (flow->application_id != ~0)
    {
      upf_adf_app_t *app =
	pool_elt_at_index (sm->upf_apps, flow->application_id);
      app_name = format (0, "%v", app->name);
    }
  else
    app_name = format (0, "%s", "None");

  s = format (s, "%U, UL pkt %u, DL pkt %u, "
	      "Forward PDR %u, Reverse PDR %u, "
	      "app %v, lifetime %u, proxy %d, spliced %d",
	      format_flow_key, &flow->key,
	      flow->stats[is_reverse].pkts,
	      flow->stats[is_reverse ^ FT_REVERSE].pkts,
	      flow_pdr_id (flow, FT_ORIGIN),
	      flow_pdr_id (flow, FT_REVERSE), app_name, flow->lifetime,
	      flow->is_l3_proxy, flow->is_spliced);
#if CLIB_DEBUG > 1
  s = format (s, ", cpu %u", flow->cpu_index);
#endif

  vec_free (app_name);
  return s;
}

static clib_error_t *
vnet_upf_flow_timeout_update (flowtable_timeout_type_t type, u16 timeout)
{
  return flowtable_lifetime_update (type, timeout);
}

int
upf_flow_timeout_update (flowtable_timeout_type_t type, u16 timeout)
{
  int rv = 0;

  vnet_upf_flow_timeout_update (type, timeout);

  return rv;
}

static u16
vnet_upf_get_flow_timeout (flowtable_timeout_type_t type)
{
  return flowtable_lifetime_get (type);
}

static clib_error_t *
upf_flow_timeout_command_fn (vlib_main_t * vm,
			     unformat_input_t * input,
			     vlib_cli_command_t * cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  u16 timeout = 0;
  clib_error_t *error = NULL;
  flowtable_timeout_type_t type = FT_TIMEOUT_TYPE_UNKNOWN;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return error;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "ip4 %u", &timeout))
	{
	  type = FT_TIMEOUT_TYPE_IPV4;
	  break;
	}
      if (unformat (line_input, "ip6 %u", &timeout))
	{
	  type = FT_TIMEOUT_TYPE_IPV6;
	  break;
	}
      if (unformat (line_input, "icmp %u", &timeout))
	{
	  type = FT_TIMEOUT_TYPE_ICMP;
	  break;
	}
      if (unformat (line_input, "udp %u", &timeout))
	{
	  type = FT_TIMEOUT_TYPE_UDP;
	  break;
	}
      if (unformat (line_input, "tcp %u", &timeout))
	{
	  type = FT_TIMEOUT_TYPE_TCP;
	  break;
	}
      else
	{
	  error = unformat_parse_error (line_input);
	  goto done;
	}
    }

  error = vnet_upf_flow_timeout_update (type, timeout);

done:
  unformat_free (line_input);

  return error;
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (upf_flow_timeout_command, static) =
{
  .path = "upf flow timeout",
  .short_help = "upf flow timeout (default | ip4 | ip6 | icmp | udp | tcp) <seconds>",
  .function = upf_flow_timeout_command_fn,
};
/* *INDENT-ON* */

static clib_error_t *
upf_show_flow_timeout_command_fn (vlib_main_t * vm,
				  unformat_input_t * input,
				  vlib_cli_command_t * cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  u16 timeout = 0;
  clib_error_t *error = NULL;
  flowtable_timeout_type_t type = FT_TIMEOUT_TYPE_UNKNOWN;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return error;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "ip4"))
	{
	  type = FT_TIMEOUT_TYPE_IPV4;
	  break;
	}
      if (unformat (line_input, "ip6"))
	{
	  type = FT_TIMEOUT_TYPE_IPV6;
	  break;
	}
      if (unformat (line_input, "icmp"))
	{
	  type = FT_TIMEOUT_TYPE_ICMP;
	  break;
	}
      if (unformat (line_input, "udp"))
	{
	  type = FT_TIMEOUT_TYPE_UDP;
	  break;
	}
      if (unformat (line_input, "tcp"))
	{
	  type = FT_TIMEOUT_TYPE_TCP;
	  break;
	}
      else
	{
	  error = unformat_parse_error (line_input);
	  goto done;
	}
    }

  timeout = vnet_upf_get_flow_timeout (type);
  vlib_cli_output (vm, "%u", timeout);

done:
  unformat_free (line_input);

  return error;
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (upf_show_flow_timeout_command, static) =
{
  .path = "show upf flow timeout",
  .short_help = "upf flow timeout (default | ip4 | ip6 | icmp | udp | tcp)",
  .function = upf_show_flow_timeout_command_fn,
};
/* *INDENT-ON* */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
