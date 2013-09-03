/* Copyright (c) 2011, TrafficLab, Ericsson Research, Hungary
 * Copyright (c) 2012, CPqD, Brazil
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the Ericsson Research nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <sys/types.h>
#include <stdbool.h>
#include <stdlib.h>

#include "action_set.h"
#include "compiler.h"
#include "dp_actions.h"
#include "dp_buffers.h"
#include "dp_exp.h"
#include "dp_ports.h"
#include "datapath.h"
#include "packet.h"
#include "pipeline.h"
#include "flow_table.h"
#include "flow_entry.h"
#include "meter_table.h"
#include "oflib/ofl.h"
#include "oflib/ofl-structs.h"
#include "nbee_link/nbee_link.h"
#include "util.h"
#include "hash.h"
#include "oflib/oxm-match.h"
#include "vlog.h"


#define LOG_MODULE VLM_pipeline

static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(60, 60);

static void
execute_entry(struct pipeline *pl, struct flow_entry *entry,
              struct flow_table **table, struct packet **pkt);

struct pipeline *
pipeline_create(struct datapath *dp) {
    struct pipeline *pl;
    int i;

    pl = xmalloc(sizeof(struct pipeline));
    for (i=0; i<PIPELINE_TABLES; i++) {
        pl->tables[i] = flow_table_create(dp, i);
    }
    pl->dp = dp;

    nblink_initialize();

    return pl;
}

static bool
is_table_miss(struct flow_entry *entry){

    return ((entry->stats->priority) == 0 && (entry->match->length <= 4));

}

/* Sends a packet to the controller in a packet_in message */
static void
send_packet_to_controller(struct pipeline *pl, struct packet *pkt, uint8_t table_id, uint8_t reason) {

    struct ofl_msg_packet_in msg;
    struct ofl_match *m;
    msg.header.type = OFPT_PACKET_IN;
    msg.total_len   = pkt->buffer->size;
    msg.reason      = reason;
    msg.table_id    = table_id;
    msg.cookie      = 0xffffffffffffffff;
    msg.data = pkt->buffer->data;


    /* A max_len of OFPCML_NO_BUFFER means that the complete
        packet should be sent, and it should not be buffered.*/
    if (pl->dp->config.miss_send_len != OFPCML_NO_BUFFER){
        dp_buffers_save(pl->dp->buffers, pkt);
        msg.buffer_id   = pkt->buffer_id;
        msg.data_length = MIN(pl->dp->config.miss_send_len, pkt->buffer->size);
    }else {
        msg.buffer_id   = OFP_NO_BUFFER;
        msg.data_length = pkt->buffer->size;
    }

    m = &pkt->handle_std->match;
    /* In this implementation the fields in_port and in_phy_port
        always will be the same, because we are not considering logical
        ports                                 */
    msg.match = (struct ofl_match_header*)m;
    dp_send_message(pl->dp, (struct ofl_msg_header *)&msg, NULL);
    ofl_structs_free_match((struct ofl_match_header* ) m, NULL);
}

void
pipeline_process_packet(struct pipeline *pl, struct packet *pkt) {
    struct flow_table *table, *next_table;

    if (VLOG_IS_DBG_ENABLED(LOG_MODULE)) {
        char *pkt_str = packet_to_string(pkt);
        VLOG_DBG_RL(LOG_MODULE, &rl, "processing packet: %s", pkt_str);
        free(pkt_str);
    }

    if (!packet_handle_std_is_ttl_valid(pkt->handle_std)) {
        if ((pl->dp->config.flags & OFPC_INVALID_TTL_TO_CONTROLLER) != 0) {
            VLOG_DBG_RL(LOG_MODULE, &rl, "Packet has invalid TTL, sending to controller.");

            send_packet_to_controller(pl, pkt, 0/*table_id*/, OFPR_INVALID_TTL);
        } else {
            VLOG_DBG_RL(LOG_MODULE, &rl, "Packet has invalid TTL, dropping.");
        }
        packet_destroy(pkt);
        return;
    }

    next_table = pl->tables[0];
    while (next_table != NULL) {
        struct flow_entry *entry;

        VLOG_DBG_RL(LOG_MODULE, &rl, "trying table %u.", next_table->stats->table_id);

        pkt->table_id = next_table->stats->table_id;
        table         = next_table;
        next_table    = NULL;

        // EEDBEH: additional printout to debug table lookup
        if (VLOG_IS_DBG_ENABLED(LOG_MODULE)) {
            char *m = ofl_structs_match_to_string((struct ofl_match_header*)&(pkt->handle_std->match), pkt->dp->exp);
            VLOG_DBG_RL(LOG_MODULE, &rl, "searching table entry for packet match: %s.", m);
            free(m);
        }
        entry = flow_table_lookup(table, pkt);
        if (entry != NULL) {
	        if (VLOG_IS_DBG_ENABLED(LOG_MODULE)) {
                char *m = ofl_structs_flow_stats_to_string(entry->stats, pkt->dp->exp);
                VLOG_DBG_RL(LOG_MODULE, &rl, "found matching entry: %s.", m);
                free(m);
            }
            pkt->handle_std->table_miss = is_table_miss(entry);
            execute_entry(pl, entry, &next_table, &pkt);
            /* Packet could be destroyed by a meter instruction */
            if (!pkt)
                return;

            if (next_table == NULL) {
               /* Cookie field is set 0xffffffffffffffff
                because we cannot associate it to any
                particular flow */
                action_set_execute(pkt->action_set, pkt, 0xffffffffffffffff, OFPR_ACTION_SET);
                packet_destroy(pkt);
                return;
            }

        } else {
			/* OpenFlow 1.3 default behavior on a table miss */
			VLOG_DBG_RL(LOG_MODULE, &rl, "No matching entry found. Dropping packet.");
			packet_destroy(pkt);
			return;
        }
    }
    VLOG_WARN_RL(LOG_MODULE, &rl, "Reached outside of pipeline processing cycle.");
}

static
int inst_compare(const void *inst1, const void *inst2){
    struct ofl_instruction_header * i1 = *(struct ofl_instruction_header **) inst1;
    struct ofl_instruction_header * i2 = *(struct ofl_instruction_header **) inst2;
    if ((i1->type == OFPIT_APPLY_ACTIONS && i2->type == OFPIT_CLEAR_ACTIONS) ||
        (i1->type == OFPIT_CLEAR_ACTIONS && i2->type == OFPIT_APPLY_ACTIONS))
        return i1->type > i2->type;

    return i1->type < i2->type;
}

ofl_err
pipeline_handle_flow_mod(struct pipeline *pl, struct ofl_msg_flow_mod *msg,
                                                const struct sender *sender) {
    /* Note: the result of using table_id = 0xff is undefined in the spec.
     *       for now it is accepted for delete commands, meaning to delete
     *       from all tables */
    ofl_err error;
    size_t i;
    bool match_kept,insts_kept;

    if(sender->remote->role == OFPCR_ROLE_SLAVE)
        return ofl_error(OFPET_BAD_REQUEST, OFPBRC_IS_SLAVE);

    match_kept = false;
    insts_kept = false;

    /*Sort by execution oder*/
    qsort(msg->instructions, msg->instructions_num,
        sizeof(struct ofl_instruction_header *), inst_compare);

    // Validate actions in flow_mod
    for (i=0; i< msg->instructions_num; i++) {
        if (msg->instructions[i]->type == OFPIT_APPLY_ACTIONS ||
            msg->instructions[i]->type == OFPIT_WRITE_ACTIONS) {
            struct ofl_instruction_actions *ia = (struct ofl_instruction_actions *)msg->instructions[i];

            error = dp_actions_validate(pl->dp, ia->actions_num, ia->actions);
            if (error) {
                return error;
            }
            error = dp_actions_check_set_field_req(msg, ia->actions_num, ia->actions);
            if (error) {
                return error;
            }
        }
    }

    /* Validate match for table 61 for Longuest Prefix Match. */
    if ((msg->table_id == 61) && (msg->command == OFPFC_ADD)) {
        struct ofl_match *match = (struct ofl_match *) msg->match;
        size_t match_size = match->header.length;
	/* Examine all match fields. */
        if (match_size) {
            struct ofl_match_tlv *oxm;
            HMAP_FOR_EACH(oxm, struct ofl_match_tlv, hmap_node, &match->match_fields){                             
                /* Wildcarded destination IPv4 address. */
                if (oxm->header == OXM_OF_IPV4_DST_W) {
		    /* Extract subnet mask. */
		    uint32_t mask = oxm->value[4] << 24 | oxm->value[5] << 16 | oxm->value[6] << 8 | oxm->value[7];
		    bool found_one = false;
		    int num_zero = 32;
		    int i;
		    /* Validate that mask is contiguous. */
		    for(i = 0; i < 32; i++) {
		        int low_bit = mask & 0x1;
			if(low_bit) {
			    if(!found_one) {
			        found_one = true;
			        num_zero = i;
			    }
			} else {
			    if(found_one) {
			        /* There is a hole in the mask. */
			        return ofl_error(OFPET_BAD_MATCH, OFPBMC_BAD_NW_ADDR_MASK);
			    }
			}
			mask >>= 1;
		    }
		    VLOG_DBG(LOG_MODULE, "Mask validation : prio = %d, num_zero = %d.", msg->priority, num_zero);
		    /* Priority must be equal to length of the mask. */
		    if(msg->priority != (32 - num_zero)) {
		        return ofl_error(OFPET_FLOW_MOD_FAILED, OFPFMFC_BAD_PRIORITY);
		    }
		} else if (oxm->header == OXM_OF_IPV4_DST) {
		    /* Exact match, priority must be 32 */
		    if(msg->priority != 32) {
		        return ofl_error(OFPET_FLOW_MOD_FAILED, OFPFMFC_BAD_PRIORITY);
		    }
		}
	    }
	}
    }

    if (msg->table_id == 0xff) {
        if (msg->command == OFPFC_DELETE || msg->command == OFPFC_DELETE_STRICT) {
            size_t i;
	    struct flow_entry *flow = NULL;

            error = 0;
            for (i=0; i < PIPELINE_TABLES; i++) {
	      error = flow_table_flow_mod(pl->tables[i], msg, &match_kept, &insts_kept, &flow);
                if (error) {
                    break;
                }
            }
            if (error) {
                return error;
            } else {
                ofl_msg_free_flow_mod(msg, !match_kept, !insts_kept, pl->dp->exp);
                return 0;
            }
        } else {
            return ofl_error(OFPET_FLOW_MOD_FAILED, OFPFMFC_BAD_TABLE_ID);
        }
    } else {
        struct flow_entry *flow = NULL;

        error = flow_table_flow_mod(pl->tables[msg->table_id], msg, &match_kept, &insts_kept, &flow);
        if (error) {
            return error;
        }
	/* Table 63 is synchronised with table 62. */
	if ((msg->table_id == 62) && (msg->command == OFPFC_ADD) && (flow != NULL)) {
	    struct ofl_msg_flow_mod *slave_msg;
	    struct flow_entry *slave_flow = NULL;
	    bool slave_match_kept = false;
	    bool slave_insts_kept = false;

	    /* Duplicate message to mess with it. */
	    error = ofl_msg_clone((struct ofl_msg_header *) msg, (struct ofl_msg_header **) &slave_msg, pl->dp->exp);
	    /* Can't return an error, otherwise remote_rconn_run()
	     * will free memory we have put into the table.
	     * To generate an error, we would need to get 'flow'
	     * out of the table, but flow_entry_remove() will
	     * generate a flow-removed message.
	     * Error is unlikely, so don't bother... Jean II
	     */
	    if (!error) {
	        struct ofl_match *slave_match = (struct ofl_match *) slave_msg->match;
		size_t match_size = slave_match->header.length;
		/* Transpose the match. */
		if (match_size) {
		    struct ofl_match_tlv *oxm;
		    HMAP_FOR_EACH(oxm, struct ofl_match_tlv, hmap_node, &slave_match->match_fields){                             
		      if (oxm->header == OXM_OF_ETH_DST)
			oxm->header = OXM_OF_ETH_SRC;
		      else if (oxm->header == OXM_OF_ETH_SRC)
			oxm->header = OXM_OF_ETH_DST;
		    }
		}

	        error = flow_table_flow_mod(pl->tables[63], slave_msg, &slave_match_kept, &slave_insts_kept, &slave_flow);
		ofl_msg_free_flow_mod(slave_msg, !slave_match_kept, !slave_insts_kept, pl->dp->exp);
		if (!error) {
		  slave_flow->sync_master = flow;
		  flow->sync_slave = slave_flow;
		}
	    }
	}
        if ((msg->command == OFPFC_ADD || msg->command == OFPFC_MODIFY || msg->command == OFPFC_MODIFY_STRICT) &&
                            msg->buffer_id != NO_BUFFER) {
            /* run buffered message through pipeline */
            struct packet *pkt;

            pkt = dp_buffers_retrieve(pl->dp->buffers, msg->buffer_id);
            if (pkt != NULL) {
		      pipeline_process_packet(pl, pkt);
            } else {
                VLOG_WARN_RL(LOG_MODULE, &rl, "The buffer flow_mod referred to was empty (%u).", msg->buffer_id);
            }
        }

        ofl_msg_free_flow_mod(msg, !match_kept, !insts_kept, pl->dp->exp);
        return 0;
    }

}

ofl_err
pipeline_handle_table_mod(struct pipeline *pl,
                          struct ofl_msg_table_mod *msg,
                          const struct sender *sender) {

    size_t ti_start;
    size_t ti_stop;
    size_t ti;
    size_t mpi;
    size_t tpi;

    if(sender->remote->role == OFPCR_ROLE_SLAVE)
        return ofl_error(OFPET_BAD_REQUEST, OFPBRC_IS_SLAVE);

    if (msg->table_id == 0xff) {
        ti_start = 0;
        ti_stop  = PIPELINE_TABLES;
    } else {
        ti_start = msg->table_id;
        ti_stop  = msg->table_id;
    }

    for (ti = ti_start; ti < ti_stop; ti++) {
	struct ofl_table_desc *table_desc = pl->tables[ti]->desc;

	/* Update properties. */
	for(mpi = 0; mpi < msg->table_mod_prop_num; mpi++) {
            if(msg->props[mpi]->type == OFPTMPT_VACANCY) {
	      for(tpi = 0; tpi < table_desc->properties_num; tpi++) {
		    if(table_desc->properties[tpi]->type == OFPTMPT_VACANCY) {
                        struct ofl_table_mod_prop_vacancy *prop_vaco = (struct ofl_table_mod_prop_vacancy *) msg->props[mpi];
                        struct ofl_table_mod_prop_vacancy *prop_vacd = (struct ofl_table_mod_prop_vacancy *) table_desc->properties[tpi];
			if(prop_vaco->vacancy_down > prop_vaco->vacancy_up)
			    return ofl_error(OFPET_TABLE_FEATURES_FAILED, OFPTFFC_BAD_ARGUMENT);
			prop_vacd->vacancy_down = prop_vaco->vacancy_down;
			prop_vacd->vacancy_up = prop_vaco->vacancy_up;
			prop_vacd->down_set = ((FLOW_TABLE_MAX_ENTRIES - pl->tables[ti]->stats->active_count) * 100 / FLOW_TABLE_MAX_ENTRIES) >= prop_vacd->vacancy_up;
		    }
		}
	    }
        }

        /* Update config flag. */
        table_desc->config = msg->config;
    }

    ofl_msg_free((struct ofl_msg_header *)msg, pl->dp->exp);
    return 0;
}

ofl_err
pipeline_handle_table_features_save(struct pipeline *pl) {

    size_t i;

    for (i=0; i<PIPELINE_TABLES; i++) {
        pl->tables[i]->saved_features->config =
            pl->tables[i]->features->config;
    }
    return 0;
}

ofl_err
pipeline_handle_table_features_restore(struct pipeline *pl) {

    size_t i;

    for (i=0; i<PIPELINE_TABLES; i++) {
        pl->tables[i]->features->config =
            pl->tables[i]->saved_features->config;
    }
    return 0;
}

ofl_err
pipeline_handle_stats_request_flow(struct pipeline *pl,
                                   struct ofl_msg_multipart_request_flow *msg,
                                   const struct sender *sender) {

    struct ofl_flow_stats **stats = xmalloc(sizeof(struct ofl_flow_stats *));
    size_t stats_size = 1;
    size_t stats_num = 0;

    if (msg->table_id == 0xff) {
        size_t i;
        for (i=0; i<PIPELINE_TABLES; i++) {
            flow_table_stats(pl->tables[i], msg, &stats, &stats_size, &stats_num);
        }
    } else {
        flow_table_stats(pl->tables[msg->table_id], msg, &stats, &stats_size, &stats_num);
    }

    {
        struct ofl_msg_multipart_reply_flow reply =
                {{{.type = OFPT_MULTIPART_REPLY},
                  .type = OFPMP_FLOW, .flags = 0x0000},
                 .stats     = stats,
                 .stats_num = stats_num
                };

        dp_send_message(pl->dp, (struct ofl_msg_header *)&reply, sender);
    }

    free(stats);
    ofl_msg_free((struct ofl_msg_header *)msg, pl->dp->exp);
    return 0;
}

ofl_err
pipeline_handle_stats_request_table(struct pipeline *pl,
                                    struct ofl_msg_multipart_request_header *msg UNUSED,
                                    const struct sender *sender) {
    struct ofl_table_stats **stats;
    size_t i;

    stats = xmalloc(sizeof(struct ofl_table_stats *) * PIPELINE_TABLES);

    for (i=0; i<PIPELINE_TABLES; i++) {
        stats[i] = pl->tables[i]->stats;
    }

    {
        struct ofl_msg_multipart_reply_table reply =
                {{{.type = OFPT_MULTIPART_REPLY},
                  .type = OFPMP_TABLE, .flags = 0x0000},
                 .stats     = stats,
                 .stats_num = PIPELINE_TABLES};

        dp_send_message(pl->dp, (struct ofl_msg_header *)&reply, sender);
    }

    free(stats);
    ofl_msg_free((struct ofl_msg_header *)msg, pl->dp->exp);
    return 0;
}

ofl_err
pipeline_handle_stats_request_table_features_request(struct pipeline *pl,
                                    struct ofl_msg_multipart_request_header *msg,
                                    const struct sender *sender) {
    size_t i, j;
    struct ofl_table_features **features;
    struct ofl_msg_multipart_request_table_features *feat =
                       (struct ofl_msg_multipart_request_table_features *) msg;

    /* Check if we already received fragments of a multipart request. */
    if(sender->remote->mp_req_msg != NULL) {
      bool nomore;

      /* We can only merge requests having the same XID. */
      if(sender->xid != sender->remote->mp_req_xid)
	{
	  VLOG_ERR(LOG_MODULE, "multipart request: wrong xid (0x%X != 0x%X)", sender->xid, sender->remote->mp_req_xid);

	  /* Technically, as our buffer can only hold one pending request,
	   * this is a buffer overflow ! Jean II */
	  /* Return error. */
	  return ofl_error(OFPET_BAD_REQUEST, OFPBRC_MULTIPART_BUFFER_OVERFLOW);
	}

      VLOG_DBG(LOG_MODULE, "multipart request: merging with previous fragments (%d+%d)", ((struct ofl_msg_multipart_request_table_features *) sender->remote->mp_req_msg)->tables_num, feat->tables_num);

      /* Merge the request with previous fragments. */
      nomore = ofl_msg_merge_multipart_request_table_features((struct ofl_msg_multipart_request_table_features *) sender->remote->mp_req_msg, feat);
      sender->remote->mp_req_lasttime = time_now();

      /* Check if incomplete. */
      if(!nomore)
	return 0;

      VLOG_DBG(LOG_MODULE, "multipart request: reassembly complete (%d)", ((struct ofl_msg_multipart_request_table_features *) sender->remote->mp_req_msg)->tables_num);

      /* Use the complete request. */
      feat = (struct ofl_msg_multipart_request_table_features *) sender->remote->mp_req_msg;

#if 0
      {
	char *str;
	str = ofl_msg_to_string((struct ofl_msg_header *) feat, pl->dp->exp);
	VLOG_DBG(LOG_MODULE, "\nMerged request:\n%s\n\n", str);
	free(str);
      }
#endif

    } else {
      /* Check if the request is an initial fragment. */
      if(msg->flags & OFPMPF_REQ_MORE) {
	struct ofl_msg_multipart_request_table_features* saved_msg;

	VLOG_DBG(LOG_MODULE, "multipart request: create reassembly buffer (%d)", feat->tables_num);

	/* Create a buffer the do reassembly. */
	saved_msg = (struct ofl_msg_multipart_request_table_features*) malloc(sizeof(struct ofl_msg_multipart_request_table_features));
	saved_msg->header.header.type = OFPT_MULTIPART_REQUEST;
	saved_msg->header.type = OFPMP_TABLE_FEATURES;
	saved_msg->header.flags = 0;
	saved_msg->tables_num = 0;
	saved_msg->table_features = NULL;

	/* Save the fragment for later use. */
	ofl_msg_merge_multipart_request_table_features(saved_msg, feat);
	sender->remote->mp_req_msg = (struct ofl_msg_multipart_request_header *) saved_msg;
	sender->remote->mp_req_xid = sender->xid;
	sender->remote->mp_req_lasttime = time_now();

	return 0;
      }

      /* Non fragmented request. Nothing to do... */
      VLOG_DBG(LOG_MODULE, "multipart request: non-fragmented request (%d)", feat->tables_num);
    }

    /*Check to see if the body is empty.*/
    /* Should check merge->tables_num instead. Jean II */
    if(feat->table_features != NULL){
        /* Change tables configuration
           TODO: Remove flows*/
        /* TODO : In theory, tables missing from the request should be
	 * disabled ! Maybe we could return an error if table number not
	 * the same, like OFPTFFC_BAD_TABLE... Jean II  */
        VLOG_DBG(LOG_MODULE, "pipeline_handle_stats_request_table_features_request: updating features");
        for(i = 0; i < feat->tables_num; i++){
	    /* Obvious memory leak.
	     * Obvious memory ownership issue when non-frag requests.
	     * Jean II */
            pl->tables[feat->table_features[i]->table_id]->features = feat->table_features[i];
        }
    }

    /* Cleanup request. */
    if(sender->remote->mp_req_msg != NULL) {
      /* Can't free entire structure, we are pointing to it ! */
      //ofl_msg_free((struct ofl_msg_header *) sender->remote->mp_req_msg, NULL);
      free(sender->remote->mp_req_msg);
      sender->remote->mp_req_msg = NULL;
      sender->remote->mp_req_xid = 0;  /* Currently not needed. Jean II. */
    }

    j = 0;
    /* Query for table capabilities */
    /* Note : PIPELINE_TABLES must be multiple of 8 for this code to work.
     * Otherwise we go out of bounds and may not set the MORE flags properly.
     * Jean II */
    loop: ;
    features = (struct ofl_table_features**) xmalloc(sizeof(struct ofl_table_features *) * 8);
    for (i = 0; i < 8; i++){
        features[i] = pl->tables[j]->features;
        j++;
    }
    {
    struct ofl_msg_multipart_reply_table_features reply =
        {{{.type = OFPT_MULTIPART_REPLY},
          .type = OFPMP_TABLE_FEATURES, .flags = j == PIPELINE_TABLES? 0x00000000:OFPMPF_REPLY_MORE},
          .table_features     = features,
          .tables_num = 8};
          dp_send_message(pl->dp, (struct ofl_msg_header *)&reply, sender);
    }
    if (j < PIPELINE_TABLES){
           goto loop;
    }

    return 0;
}

ofl_err
pipeline_handle_stats_request_table_desc_request(struct pipeline *pl,
                                    struct ofl_msg_multipart_request_header *msg UNUSED,
                                    const struct sender *sender) {
    size_t i, j, pi;
    struct ofl_table_desc **desc;
    struct ofl_table_mod_prop_vacancy *prop_vac;

    j = 0;
    /* Query for table capabilities */
    loop: ;
    desc = (struct ofl_table_desc**) xmalloc(sizeof(struct ofl_table_desc *) * 16);
    for (i = 0; i < 16; i++){
        desc[i] = pl->tables[j]->desc;
	/* Update vacancy. */
	for(pi = 0; pi < desc[i]->properties_num; pi++) {
        /* modified by dingwanfu_new */
        if (desc[i]->config & OFPTC_VACANCY_EVENTS) {
	    prop_vac = (struct ofl_table_mod_prop_vacancy *) desc[i]->properties[pi];
	    if(prop_vac->type == OFPTMPT_VACANCY) {
	        prop_vac->vacancy = (FLOW_TABLE_MAX_ENTRIES - pl->tables[j]->stats->active_count) * 100 / FLOW_TABLE_MAX_ENTRIES;
	    }
	  }
        else if (desc[i]->config & OFPTC_EVICTION)
        {
            ;  /* do nothing here, just send the orginal desc, including OFPTMPT_EVICTION */
        }
	}
        j++;
    }
    {
    struct ofl_msg_multipart_reply_table_desc reply =
        {{{.type = OFPT_MULTIPART_REPLY},
          .type = OFPMP_TABLE_DESC, .flags = j == PIPELINE_TABLES? 0x00000000:OFPMPF_REPLY_MORE},
          .table_desc     = desc,
          .tables_num = 16};
          dp_send_message(pl->dp, (struct ofl_msg_header *)&reply, sender);
    }
    if (j < PIPELINE_TABLES){
           goto loop;
    }

    return 0;
}

ofl_err
pipeline_handle_stats_request_aggregate(struct pipeline *pl,
                                  struct ofl_msg_multipart_request_flow *msg,
                                  const struct sender *sender) {
    struct ofl_msg_multipart_reply_aggregate reply =
            {{{.type = OFPT_MULTIPART_REPLY},
              .type = OFPMP_AGGREGATE, .flags = 0x0000},
              .packet_count = 0,
              .byte_count   = 0,
              .flow_count   = 0};

    if (msg->table_id == 0xff) {
        size_t i;

        for (i=0; i<PIPELINE_TABLES; i++) {
            flow_table_aggregate_stats(pl->tables[i], msg,
                                       &reply.packet_count, &reply.byte_count, &reply.flow_count);
        }

    } else {
        flow_table_aggregate_stats(pl->tables[msg->table_id], msg,
                                   &reply.packet_count, &reply.byte_count, &reply.flow_count);
    }

    dp_send_message(pl->dp, (struct ofl_msg_header *)&reply, sender);

    ofl_msg_free((struct ofl_msg_header *)msg, pl->dp->exp);
    return 0;
}


void
pipeline_destroy(struct pipeline *pl) {
    struct flow_table *table;
    int i;

    for (i=0; i<PIPELINE_TABLES; i++) {
        table = pl->tables[i];
        if (table != NULL) {
            flow_table_destroy(table);
        }
    }
    free(pl);
}


void
pipeline_timeout(struct pipeline *pl) {
    int i;

    for (i = 0; i < PIPELINE_TABLES; i++) {
        flow_table_timeout(pl->tables[i]);
    }
}


/* Executes the instructions associated with a flow entry */
static void
execute_entry(struct pipeline *pl, struct flow_entry *entry,
              struct flow_table **next_table, struct packet **pkt) {
    /* NOTE: instructions, when present, will be executed in
            the following order:
            Meter
            Apply-Actions
            Clear-Actions
            Write-Actions
            Write-Metadata
            Goto-Table
    */
    size_t i;
    struct ofl_instruction_header *inst;

    for (i=0; i < entry->stats->instructions_num; i++) {
        /*Packet was dropped by some instruction or action*/

        if(!(*pkt)){
            return;
        }

        inst = entry->stats->instructions[i];
        switch (inst->type) {
            case OFPIT_GOTO_TABLE: {
                struct ofl_instruction_goto_table *gi = (struct ofl_instruction_goto_table *)inst;

                *next_table = pl->tables[gi->table_id];
                break;
            }
            case OFPIT_WRITE_METADATA: {
                struct ofl_instruction_write_metadata *wi = (struct ofl_instruction_write_metadata *)inst;
                struct  ofl_match_tlv *f;

                /* NOTE: Hackish solution. If packet had multiple handles, metadata
                 *       should be updated in all. */
                packet_handle_std_validate((*pkt)->handle_std);
                /* Search field on the description of the packet. */
                HMAP_FOR_EACH_WITH_HASH(f, struct ofl_match_tlv,
                    hmap_node, hash_int(OXM_OF_METADATA,0), &(*pkt)->handle_std->match.match_fields){
                    uint64_t *metadata = (uint64_t*) f->value;
                    *metadata = (*metadata & ~wi->metadata_mask) | (wi->metadata & wi->metadata_mask);
                    VLOG_DBG_RL(LOG_MODULE, &rl, "Executing write metadata: %llx", *metadata);
                }
                break;
            }
            case OFPIT_WRITE_ACTIONS: {
                struct ofl_instruction_actions *wa = (struct ofl_instruction_actions *)inst;
                action_set_write_actions((*pkt)->action_set, wa->actions_num, wa->actions);
                break;
            }
            case OFPIT_APPLY_ACTIONS: {
                struct ofl_instruction_actions *ia = (struct ofl_instruction_actions *)inst;
                dp_execute_action_list((*pkt), ia->actions_num, ia->actions, entry->stats->cookie, (flow_entry_is_table_miss(entry) ? OFPR_TABLE_MISS : OFPR_APPLY_ACTION) );
                break;
            }
            case OFPIT_CLEAR_ACTIONS: {
                action_set_clear_actions((*pkt)->action_set);
                break;
            }
            case OFPIT_METER: {
            	struct ofl_instruction_meter *im = (struct ofl_instruction_meter *)inst;
                meter_table_apply(pl->dp->meters, pkt , im->meter_id);
                break;
            }
            case OFPIT_EXPERIMENTER: {
                dp_exp_inst((*pkt), (struct ofl_instruction_experimenter *)inst);
                break;
            }
        }
    }
}

