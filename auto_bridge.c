/*
*   Copyright (c) 2016 Freescale Semiconductor, Inc. All rights reserved.
*   Copyright 2016 NXP
*
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <https://www.gnu.org/licenses/>.
*
*/

 
#include <linux/version.h>
#include <linux/socket.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/netlink.h>
#include <linux/timer.h>
#include <linux/time.h>
#include <linux/if_ether.h>
#include <linux/jhash.h>
#include <linux/list.h>
#include <linux/netfilter_bridge.h>
#include <net/net_namespace.h>
#include <linux/netfilter.h>
#include <linux/netfilter_bridge/ebtables.h>
#include <linux/proc_fs.h>
#include <linux/ip.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <linux/if_bridge.h>
#include <linux/workqueue.h>



#include "auto_bridge_private.h"
#include "include/auto_bridge.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mindspeed Technologies");
MODULE_DESCRIPTION("Automatic Bridging Module (ABM)");

static char __initdata auto_bridge_version[] = "0.01";

#define SECS * HZ
#define MINS * 60 SECS
#define HOURS * 60 MINS
#define DAYS * 24 HOURS

struct list_head			l2flow_table[L2FLOW_HASH_TABLE_SIZE];
struct list_head			l2flow_table_by_src_mac[L2FLOW_HASH_BY_MAC_TABLE_SIZE];
struct list_head			l2flow_table_by_dst_mac[L2FLOW_HASH_BY_MAC_TABLE_SIZE];

struct list_head			l2flow_list_wait_for_ack;
struct list_head			l2flow_list_msg_to_send;

static struct kmem_cache		*l2flow_cache /*__read_mostly*/;
static struct sock			*abm_nl = NULL;
static char			abm_l3_filtering = 0;
static unsigned int			abm_max_entries = ABM_DEFAULT_MAX_ENTRIES;
static unsigned int			abm_nb_entries =	0;
struct workqueue_struct		*kabm_wq;
static int				abm_retransmit_time = 2 SECS;

DEFINE_SPINLOCK(abm_lock);
static DECLARE_WORK(abm_work_send_msg, abm_do_work_send_msg);
static DECLARE_DELAYED_WORK(abm_work_retransmit, abm_do_work_retransmit);


static unsigned int l2flow_timeouts[L2FLOW_STATE_MAX] /*__read_mostly*/ = {
	[L2FLOW_STATE_SEEN]			= 10 SECS,
	[L2FLOW_STATE_CONFIRMED]		= 2 MINS, 
	[L2FLOW_STATE_LINUX]			= 10 SECS,
	[L2FLOW_STATE_DYING]			= 2 MINS, // This state is here to give some time for retransmission
};

static const char *const l2flow_states_string[L2FLOW_STATE_MAX] __read_mostly = {
	[L2FLOW_STATE_SEEN]			= "SEEN",
	[L2FLOW_STATE_CONFIRMED]		= "CONFIRMED", //Should not timeout here
	[L2FLOW_STATE_LINUX]			= "LINUX",	
	[L2FLOW_STATE_FF]			= "FF",		
	[L2FLOW_STATE_DYING]			= "DYING", 	

};

/***************************************************************************
*
* abm_do_work_send_msg
* Used to send delayed msg
*
****************************************************************************/
static void abm_do_work_send_msg(struct work_struct *work)
{
	struct list_head *entry, *tmp;
	struct l2flowTable *table_entry;
	char action = 0;

	if (!netlink_has_listeners(abm_nl, L2FLOW_NL_GRP)){
		return;
	}
	spin_lock_bh(&abm_lock);
	//TODO : Need to limit the number of messages to sent while holding the lock.
	list_for_each_safe(entry, tmp, &l2flow_list_msg_to_send){
		table_entry = container_of(entry, struct l2flowTable, list_msg_to_send);
		if((table_entry->state == L2FLOW_STATE_SEEN) 
		|| (table_entry->state == L2FLOW_STATE_CONFIRMED)){
			action = L2FLOW_ENTRY_NEW;		
		}
		else if((table_entry->state == L2FLOW_STATE_LINUX) 
		|| (table_entry->state == L2FLOW_STATE_FF)){
			action = L2FLOW_ENTRY_UPDATE;
		}
		else if (table_entry->state == L2FLOW_STATE_DYING){
			action = L2FLOW_ENTRY_DEL;
		}
		if(abm_nl_send_l2flow_msg(abm_nl, action, 0, table_entry) != -ENOTCONN){
			table_entry->flags &= ~L2FLOW_FL_PENDING_MSG;
			table_entry->flags &= ~L2FLOW_FL_NEEDS_UPDATE;
			list_del(&table_entry->list_msg_to_send);
			table_entry->time_sent = jiffies;
			if(!(table_entry->flags & L2FLOW_FL_WAIT_ACK)){
				list_add(&table_entry->list_wait_for_ack, &l2flow_list_wait_for_ack);
				table_entry->flags |= L2FLOW_FL_WAIT_ACK;
			}
		}
	}
	spin_unlock_bh(&abm_lock);
}

/***************************************************************************
*
* abm_do_work_retransmit
* Periodic work used to check and re-transmit messages
*
****************************************************************************/
static void abm_do_work_retransmit(struct work_struct *work)
{
	struct list_head *entry;
	struct l2flowTable *table_entry;
	char action = 0;

	spin_lock_bh(&abm_lock);
	
	if(list_empty(&l2flow_list_wait_for_ack))
		goto resched;
			
	if (!netlink_has_listeners(abm_nl, L2FLOW_NL_GRP))
		goto resched;
	

	list_for_each(entry, &l2flow_list_wait_for_ack){
		table_entry = container_of(entry, struct l2flowTable, list_wait_for_ack);
		if(time_is_before_jiffies(table_entry->time_sent + abm_retransmit_time)){
			if((table_entry->state == L2FLOW_STATE_SEEN) 
			|| (table_entry->state == L2FLOW_STATE_CONFIRMED)){
				action = L2FLOW_ENTRY_NEW;		
			}
			else if((table_entry->state == L2FLOW_STATE_LINUX) 
			|| (table_entry->state == L2FLOW_STATE_FF)){
				action = L2FLOW_ENTRY_UPDATE;
			}
			else if (table_entry->state == L2FLOW_STATE_DYING){
				action = L2FLOW_ENTRY_DEL;
			}
			if (!abm_nl_send_l2flow_msg(abm_nl, action, 0, table_entry)){
				/* Success : Update time and continue to next entry */
				table_entry->time_sent = jiffies;
			}
			else /* Otherwise don't spend more time here and wait some more time */
				goto resched;
		}
	}
resched:
	spin_unlock_bh(&abm_lock);
	queue_delayed_work(kabm_wq, &abm_work_retransmit, abm_retransmit_time);
}
/***************************************************************************
*
* abm_br_event
* The bridge notifier callback.
* Can be called from mostly any context. Events to user-space are not sent here.
*
****************************************************************************/
int abm_br_event(struct notifier_block *unused, unsigned long event, void *ptr){
	int work_to_do = 0;

	if (event == BREVENT_PORT_DOWN) {
		struct net_device * dev = (struct net_device *) ptr;
		int i;
		struct list_head *entry;
		struct l2flowTable *table_entry;

		for (i = 0; i < L2FLOW_HASH_TABLE_SIZE; i++) {
			spin_lock_bh(&abm_lock);
			list_for_each(entry,  &l2flow_table[i]) {
				table_entry = container_of(entry, struct l2flowTable, list);
				if ((table_entry->state != L2FLOW_STATE_DYING)
				&& ((table_entry->idev_ifi == dev->ifindex) || (table_entry->odev_ifi == dev->ifindex))){
					unsigned int  no_timer = (table_entry->state == L2FLOW_STATE_FF);	
					table_entry->state = L2FLOW_STATE_DYING;

				if (!(table_entry->flags & L2FLOW_FL_PENDING_MSG)) {
						table_entry->flags |= L2FLOW_FL_PENDING_MSG;
						list_add(&table_entry->list_msg_to_send, &l2flow_list_msg_to_send);
						work_to_do = 1;
				}
				if (del_timer(&table_entry->timeout) || no_timer)
					__abm_go_dying(table_entry);
				}
			}
			spin_unlock_bh(&abm_lock);
		}
	}
	else if (event == BREVENT_FDB_UPDATE){
		struct brevent_fdb_update * fdb_update;
		int key;
		struct list_head *entry;
		struct l2flowTable *table_entry;

		fdb_update = (struct brevent_fdb_update *) ptr;
		key = abm_l2flow_hash_mac(fdb_update->mac_addr);

		spin_lock_bh(&abm_lock);
		list_for_each(entry,  &l2flow_table_by_dst_mac[key]){
			table_entry = container_of(entry, struct l2flowTable, list_by_dst_mac);
			/* Send the event in every other state different than DYING */
			/* There is no issue sending more events than needed */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,6,0)
			if (ether_addr_equal(fdb_update->mac_addr, table_entry->l2flow.daddr)
			&& (fdb_update->dev->ifindex != table_entry->odev_ifi)
			&& (table_entry->state != L2FLOW_STATE_DYING)) 
#else
			if (!compare_ether_addr(fdb_update->mac_addr, table_entry->l2flow.daddr)
			&& (fdb_update->dev->ifindex != table_entry->odev_ifi)
			&& (table_entry->state != L2FLOW_STATE_DYING)) 
#endif
			{
				table_entry->odev_ifi = fdb_update->dev->ifindex;

				if (!(table_entry->flags & L2FLOW_FL_PENDING_MSG)) {
					table_entry->flags |= L2FLOW_FL_PENDING_MSG;
					list_add(&table_entry->list_msg_to_send, &l2flow_list_msg_to_send);
					work_to_do = 1;
				}
			}
		}
		spin_unlock_bh(&abm_lock);
	}
	if(work_to_do)
		queue_work(kabm_wq, &abm_work_send_msg);

	return NOTIFY_DONE;
}
struct notifier_block abm_br_notifier = {
	.notifier_call = abm_br_event
};


/***************************************************************************
*
* abm_fdb_can_expire
* Callback registered in br_fb. 
* Return 0 if an entry is fast-forwarded, 1 otherwise
*
****************************************************************************/
int abm_fdb_can_expire(unsigned char *mac_addr, struct net_device *dev)
{
	int key;
	struct l2flowTable *table_entry;
	struct list_head *entry;

	key = abm_l2flow_hash_mac(mac_addr);

	spin_lock(&abm_lock);
	list_for_each(entry,  &l2flow_table_by_src_mac[key]){
		table_entry = container_of(entry, struct l2flowTable, list_by_src_mac);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,6,0)
		if (ether_addr_equal(mac_addr, table_entry->l2flow.saddr)
		&& (dev->ifindex == table_entry->idev_ifi))
#else
		if(!compare_ether_addr(mac_addr, table_entry->l2flow.saddr)
		&& (dev->ifindex == table_entry->idev_ifi))
#endif
		{
			if(table_entry->state == L2FLOW_STATE_FF){
				spin_unlock(&abm_lock);
				return 0;
			}
		}
	}
	spin_unlock(&abm_lock);
	return 1;
}
static inline size_t abm_l2flow_msg_size(void)
{
	return NLMSG_ALIGN(sizeof(struct l2flow_msg))
		+ nla_total_size(sizeof(u32))		/* L2FLOWA_SVLAN_TAG */
		+ nla_total_size(sizeof(u32))		/* L2FLOWA_CVLAN_TAG */
		+ nla_total_size(sizeof(u32))		/* L2FLOWA_PPP_S_ID */
		+ nla_total_size(sizeof(u32))		/* L2FLOWA_IIF_IDX */
		+ nla_total_size(sizeof(u32))		/* L2FLOWA_OIF_DIX */
		+ nla_total_size(4 * sizeof(u32))		/* L2FLOWA_IP_SRC */
		+ nla_total_size(4 * sizeof(u32))		/* L2FLOWA_IP_DST */
		+ nla_total_size(sizeof(u8))		/* L2FLOWA_IP_PROTO */
		+ nla_total_size(sizeof(u16))		/* L2FLOWA_SPORT */
		+ nla_total_size(sizeof(u16))		/* L2FLOWA_DPORT */
		+ nla_total_size(sizeof(u16))		/* L2FLOWA_MARK */
		;
}

/***************************************************************************
*
* abm_nl_send_rst_msg
* Send L2FLOW_MSG_RESET msg types to user-space
* 
****************************************************************************/
static int abm_nl_send_rst_msg(struct sock *s)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	int err = 0;
	
	skb = nlmsg_new(0, GFP_KERNEL);
	if(skb == NULL){
		err = -ENOMEM;
		goto err;
	}
	
	nlh = nlmsg_put(skb, 0, 0, L2FLOW_MSG_RESET, 0, 0);
	if(nlh == NULL){
		err = -ENOMEM;
		goto err2;
	}
	
	nlmsg_end(skb, nlh);

	if (netlink_has_listeners(s, L2FLOW_NL_GRP)){
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,7,0)
		NETLINK_CB(skb).portid = 0;	/* from kernel */
#else
		NETLINK_CB(skb).pid = 0;	/* from kernel */
#endif
		NETLINK_CB(skb).dst_group = L2FLOW_NL_GRP;

		return netlink_broadcast(s, skb, 0, L2FLOW_NL_GRP, GFP_KERNEL);

	}
	else{
		err = -ENOTCONN;
		goto err2;
	}

err2:
	kfree_skb(skb);
err:
	return err;
}

/***************************************************************************
*
* abm_nl_send_l2flow_msg
* Send L2FLOW_MSG_ENTRY msg types to user-space
* 
****************************************************************************/
static int abm_nl_send_l2flow_msg(struct sock *s, char action, int flags, struct l2flowTable *table_entry)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct l2flow_msg *l2flow_msg;
	int err = 0;

	skb = nlmsg_new(abm_l2flow_msg_size(), GFP_ATOMIC);
	if(skb == NULL){
		err = -ENOMEM;
		goto err;
	}
	
	nlh = nlmsg_put(skb, 0, 0, L2FLOW_MSG_ENTRY, sizeof(*l2flow_msg), 0);
	if(nlh == NULL){
		err = -ENOMEM;
		goto err2;
	}

	l2flow_msg = nlmsg_data(nlh);
	l2flow_msg->action = action;
	l2flow_msg->flags = flags;
	memcpy(l2flow_msg->saddr, table_entry->l2flow.saddr, 6);
	memcpy(l2flow_msg->daddr, table_entry->l2flow.daddr, 6);
	l2flow_msg->ethertype = table_entry->l2flow.ethertype;

	NLA_PUT_U32(skb, L2FLOWA_IIF_IDX, table_entry->idev_ifi);
	NLA_PUT_U32(skb, L2FLOWA_OIF_IDX, table_entry->odev_ifi);
	NLA_PUT_U16(skb, L2FLOWA_MARK, table_entry->packet_mark);

	if(table_entry->l2flow.ethertype == htons(ETH_P_8021Q))
	{
		NLA_PUT_U16(skb, L2FLOWA_SVLAN_TAG, table_entry->l2flow.svlan_tag);
		NLA_PUT_U16(skb, L2FLOWA_CVLAN_TAG, table_entry->l2flow.cvlan_tag);
	}
	else if (table_entry->l2flow.ethertype == htons(ETH_P_PPP_SES))
		NLA_PUT_U16(skb, L2FLOWA_PPP_S_ID, table_entry->l2flow.session_id);

	if(abm_l3_filtering){

		if(table_entry->l2flow.ethertype != htons(ETH_P_PPP_SES))
			NLA_PUT_U8(skb, L2FLOWA_IP_PROTO, table_entry->l2flow.l3.proto);
		
		if(table_entry->l2flow.ethertype == htons(ETH_P_IP)){
			NLA_PUT_U32(skb, L2FLOWA_IP_SRC, table_entry->l2flow.l3.saddr.ip);
			NLA_PUT_U32(skb, L2FLOWA_IP_DST, table_entry->l2flow.l3.daddr.ip);
		}
		else if (table_entry->l2flow.ethertype == htons(ETH_P_IPV6)){
			NLA_PUT(skb, L2FLOWA_IP_SRC, sizeof(u_int32_t) * 4, 
					table_entry->l2flow.l3.saddr.ip6);
			NLA_PUT(skb, L2FLOWA_IP_DST, sizeof(u_int32_t) * 4, 
					table_entry->l2flow.l3.daddr.ip6);
		}
		
		if((table_entry->l2flow.l3.proto == IPPROTO_UDP) 
		|| (table_entry->l2flow.l3.proto == IPPROTO_TCP)){
			NLA_PUT_U16(skb, L2FLOWA_SPORT, table_entry->l2flow.l4.sport);
			NLA_PUT_U16(skb, L2FLOWA_DPORT, table_entry->l2flow.l4.dport);
		}
	}
	nlmsg_end(skb, nlh);
	
	if (netlink_has_listeners(s, L2FLOW_NL_GRP)){
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,7,0)
		NETLINK_CB(skb).portid = 0;	/* from kernel */
#else
		NETLINK_CB(skb).pid = 0;	/* from kernel */
#endif
		NETLINK_CB(skb).dst_group = L2FLOW_NL_GRP;

		return netlink_broadcast(s, skb, 0, L2FLOW_NL_GRP, GFP_ATOMIC);
	}
	else{
		err = -ENOTCONN;
		goto err2;
	}
	
nla_put_failure:
	nlmsg_cancel(skb, nlh);
	err = -EMSGSIZE;
err2:
	kfree_skb(skb);
err:
	return err;

}

/***************************************************************************
*
* abm_nl_rcv_msg
* Handle NL message from user-space
* 
****************************************************************************/
static int abm_nl_rcv_msg(struct sk_buff *skb, struct nlmsghdr *nlh ,struct netlink_ext_ack *ext )
{
	int type, err = 0;
	struct l2flow l2flow_temp;
	struct l2flow_msg *l2flow_msg;
	struct nlattr *tb[L2FLOWA_MAX + 1];

	type = nlh->nlmsg_type;

	if(type >= L2FLOW_MSG_MAX){
		err = -EAGAIN;
		goto out;
	}

	err = nlmsg_parse(nlh, sizeof(*l2flow_msg), tb, L2FLOWA_MAX, NULL,NULL);
	if(err < 0)
		goto out;
	
	switch(type)
	{
		case L2FLOW_MSG_ENTRY:
			/*Messages must have at least l2flow_msg length */
			if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(struct l2flow_msg))){
				err = -EAGAIN;
				goto out;
			}
	
			memset(&l2flow_temp, 0, sizeof(l2flow_temp));
			l2flow_msg = NLMSG_DATA(nlh);
			
			/* Here we don't really care of message sanity, if parameters are wrong entry won't be found */
			/* No entry is created here */
			memcpy(l2flow_temp.saddr, l2flow_msg->saddr, ETH_ALEN);
			memcpy(l2flow_temp.daddr, l2flow_msg->daddr, ETH_ALEN);
			l2flow_temp.ethertype = l2flow_msg->ethertype;

			if(tb[L2FLOWA_SVLAN_TAG]) {
				l2flow_temp.svlan_tag = nla_get_u16(tb[L2FLOWA_SVLAN_TAG]);
			}
			if(tb[L2FLOWA_CVLAN_TAG]) {
				l2flow_temp.cvlan_tag = nla_get_u16(tb[L2FLOWA_CVLAN_TAG]);
			}

			if(tb[L2FLOWA_PPP_S_ID])
				l2flow_temp.session_id = nla_get_u16(tb[L2FLOWA_PPP_S_ID]);

			if(tb[L2FLOWA_IP_SRC])
				memcpy(&l2flow_temp.l3.saddr.all, nla_data(tb[L2FLOWA_IP_SRC]), nla_len(tb[L2FLOWA_IP_SRC]));

			if(tb[L2FLOWA_IP_DST])
				memcpy(&l2flow_temp.l3.daddr.all, nla_data(tb[L2FLOWA_IP_DST]), nla_len(tb[L2FLOWA_IP_DST]));

			if(tb[L2FLOWA_IP_PROTO])
				l2flow_temp.l3.proto= nla_get_u8(tb[L2FLOWA_IP_PROTO]);
			
			if(tb[L2FLOWA_SPORT])
				l2flow_temp.l4.sport= nla_get_u16(tb[L2FLOWA_SPORT]);

			if(tb[L2FLOWA_DPORT])
				l2flow_temp.l4.dport= nla_get_u16(tb[L2FLOWA_DPORT]);
			
			err = abm_l2flow_msg_handle(l2flow_msg->action, l2flow_msg->flags, &l2flow_temp);
			
			
		break;
		case L2FLOW_MSG_RESET:
		break;	
	}
out:
	return err;
}

static void abm_nl_rcv_skb(struct sk_buff *skb)
{
	netlink_rcv_skb(skb, &abm_nl_rcv_msg);
}
/***************************************************************************
*
* __abm_go_dying
* Move an entry to the the L2FLOW_STATE_DYING or delete the entry depending
* on L2FLOW_FL_DEAD flag.
****************************************************************************/
static void __abm_go_dying(struct l2flowTable *table_entry)
{
	/* This function can be called from bridge event notifier, timer and netlink */
	if(!(table_entry->flags & L2FLOW_FL_DEAD)){

		/* Skip Netlink message sending if already pending but if we come from another state send it anyway */
		if(!(table_entry->flags & L2FLOW_FL_PENDING_MSG)  || (table_entry->state != L2FLOW_STATE_DYING))
			if(abm_nl_send_l2flow_msg(abm_nl, L2FLOW_ENTRY_DEL, 0, table_entry) != -ENOTCONN){
				/* If message is succesully sent we expect an ack */
				table_entry->flags |= L2FLOW_FL_WAIT_ACK;
				list_add(&table_entry->list_wait_for_ack, &l2flow_list_wait_for_ack);
			}
		table_entry->state = L2FLOW_STATE_DYING;
		table_entry->flags |= L2FLOW_FL_DEAD;
		table_entry->timeout.expires = jiffies + l2flow_timeouts[L2FLOW_STATE_DYING];
		add_timer(&table_entry->timeout);
	}
	else // Really die :)
	{
		abm_l2flow_del(table_entry);
	}
}

/***************************************************************************
*
* abm_death_by_timeout
* Timers callback
*
****************************************************************************/
static void  abm_death_by_timeout(unsigned long arg)
{
	struct l2flowTable *table_entry = (struct l2flowTable *)arg;
	
	spin_lock_bh(&abm_lock);
	__abm_go_dying(table_entry);
	spin_unlock_bh(&abm_lock);
}


static int abm_nl_init(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,7,0)
	struct netlink_kernel_cfg cfg = {
		.groups	  = L2FLOW_NL_GRP,
		.input	  = abm_nl_rcv_skb,
	};
	abm_nl = netlink_kernel_create(&init_net, NETLINK_L2FLOW, &cfg);
#else
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,33)
	if((abm_nl = netlink_kernel_create (&init_net, NETLINK_L2FLOW, L2FLOW_NL_GRP, 
				abm_nl_rcv_skb, NULL, THIS_MODULE)) == 0)
		return -ENOMEM;
						
#endif
#endif

	return 0;
}
static void abm_nl_exit(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,33)
	netlink_kernel_release(abm_nl);
#endif
}
/***************************************************************************
*
* abm_l2flow_cmp
* Compare two L2 flows
*
****************************************************************************/
static inline int abm_l2flow_cmp(struct l2flow *flow_a, struct l2flow *flow_b)
{
	return memcmp(flow_a, flow_b, sizeof(struct l2flow));
}

/***************************************************************************
*
* abm_l2flow_find
* Find a L2 flow table entry from a temporary L2 flow
*
****************************************************************************/
static struct l2flowTable * abm_l2flow_find(struct l2flow *l2flowtmp)
{
	int key;
	struct list_head *entry;
	struct l2flowTable *table_entry;
	
	key = abm_l2flow_hash(l2flowtmp);

	list_for_each(entry,  &l2flow_table[key]){
		table_entry = container_of(entry, struct l2flowTable, list);
		if(!abm_l2flow_cmp(&table_entry->l2flow, l2flowtmp))
			return table_entry; // Found
	}
	return NULL; // Not found 
}

/***************************************************************************
*
* abm_l2flow_update
* Update L2 flow entry state when status received from user-space
*
****************************************************************************/
static void abm_l2flow_update(int flags, struct l2flowTable *table_entry)
{
	if(flags & L2FLOW_OFFLOADED){
		/* Flow is programmed in FPP */
		table_entry->state = L2FLOW_STATE_FF;
		/* If timer already expired we'll die, it's ok though... */
		del_timer(&table_entry->timeout);
	}
	else if(flags & L2FLOW_DENIED){
		/* Flow is not programmed in FPP */
		table_entry->state = L2FLOW_STATE_LINUX;
		mod_timer(&table_entry->timeout, jiffies + l2flow_timeouts[table_entry->state]);
	}
	if(table_entry->flags & L2FLOW_FL_WAIT_ACK){
		table_entry->flags &= ~L2FLOW_FL_WAIT_ACK;
		list_del(&table_entry->list_wait_for_ack);
	}
}

/***************************************************************************
*
* abm_l2flow_del
* Remove entry from table and free entry
*
****************************************************************************/
static void abm_l2flow_del(struct l2flowTable *table_entry)
{
	list_del(&table_entry->list);
	list_del(&table_entry->list_by_src_mac);
	list_del(&table_entry->list_by_dst_mac);
	if(table_entry->flags & L2FLOW_FL_PENDING_MSG)
		list_del(&table_entry->list_msg_to_send);
	if(table_entry->flags & L2FLOW_FL_WAIT_ACK)
		list_del(&table_entry->list_wait_for_ack);
	kmem_cache_free(l2flow_cache, table_entry);
	abm_nb_entries--;
}

/***************************************************************************
*
* abm_l2flow_msg_handle
* Handle Netlink messages from user-space
*
****************************************************************************/
static int abm_l2flow_msg_handle(char action, int flags, struct l2flow *l2flowtmp)
{
	struct l2flowTable *table_entry = NULL;
	int rc = 0;
	spin_lock_bh(&abm_lock);
	
	table_entry = abm_l2flow_find(l2flowtmp);

	if(!table_entry){
		rc = -ENOENT;
		goto out;
	}

	if(action == L2FLOW_ENTRY_UPDATE){
		abm_l2flow_update(flags, table_entry);
	}
	else if(action == L2FLOW_ENTRY_DEL){
		/* No need to wait in dying state as event is coming from user-space app */
		table_entry->flags |= L2FLOW_FL_DEAD;

		if(table_entry->flags & L2FLOW_FL_WAIT_ACK){
			table_entry->flags &= ~L2FLOW_FL_WAIT_ACK;
			list_del(&table_entry->list_wait_for_ack);
		}

		/* Die soon or now */
		if(del_timer(&table_entry->timeout) || (table_entry->state == L2FLOW_STATE_FF))
			__abm_go_dying(table_entry);
	}
	else{
		rc = -ENOMSG;
		goto out;
	}
out:
	spin_unlock_bh(&abm_lock);
	return rc;
}

/***************************************************************************
*
* abm_l2flow_add
* This function allocates and add an entry into flow_table from a temporary l2flow
*
****************************************************************************/
static struct l2flowTable * abm_l2flow_add(struct l2flow *l2flowtmp)
{
	unsigned int key, key_src_mac, key_dst_mac;
	struct l2flowTable* l2flow_entry = NULL;

	if(abm_nb_entries >= abm_max_entries)
		goto out;

	key = abm_l2flow_hash(l2flowtmp);
	key_src_mac = abm_l2flow_hash_mac(l2flowtmp->saddr);
	key_dst_mac = abm_l2flow_hash_mac(l2flowtmp->daddr);
	
	l2flow_entry = kmem_cache_alloc(l2flow_cache, GFP_ATOMIC); // called under soft_irq context
	if(!l2flow_entry){
		printk(KERN_ERR "Automatic bridging module error l2flow_cache OOM\n");
		goto out;
	}
	memset(l2flow_entry, 0, sizeof(*l2flow_entry));
	memcpy(&l2flow_entry->l2flow, l2flowtmp, sizeof(*l2flowtmp));
	/* Timer not yet started here */
	setup_timer(&l2flow_entry->timeout, abm_death_by_timeout, (unsigned long) l2flow_entry);
	
	list_add(&l2flow_entry->list, &l2flow_table[key]);
	list_add(&l2flow_entry->list_by_src_mac, &l2flow_table_by_src_mac[key_src_mac]);
	list_add(&l2flow_entry->list_by_dst_mac, &l2flow_table_by_dst_mac[key_dst_mac]);

	abm_nb_entries++;
out:
	return l2flow_entry;
}

struct tcpudphdr {
	__be16 src;
	__be16 dst;
};

/***************************************************************************
*
* abm_build_l2flow
* Build the temporary L2 flow
*
****************************************************************************/
static inline int abm_build_l2flow(struct sk_buff *skb, struct l2flow *l2flow_temp, unsigned short ethertype)
{
	memcpy(l2flow_temp->saddr, eth_hdr(skb)->h_source, ETH_ALEN);
	memcpy(l2flow_temp->daddr, eth_hdr(skb)->h_dest, ETH_ALEN);
	l2flow_temp->ethertype = ethertype;

	if(ethertype == htons(ETH_P_8021Q)){
		struct vlan_hdr *vlanh;
		struct vlan_hdr _vlanh;
		struct vlan_ethhdr *vlan_eth_h;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,0,0)
		if (skb_vlan_tag_present(skb)) {
			l2flow_temp->svlan_tag = htons(skb_vlan_tag_get(skb));
#else
		if (vlan_tx_tag_present(skb)) {
			l2flow_temp->svlan_tag = htons(vlan_tx_tag_get(skb));
#endif
			if ((vlan_eth_h = vlan_eth_hdr(skb))!= NULL) {
			/* vlan_eth_h->h_vlan_proto is not matched ETH_P_8021Q then cvlan_tag set to 0 (already initialized to 0)*/
				if (vlan_eth_h->h_vlan_proto == htons(ETH_P_8021Q)) {
					l2flow_temp->cvlan_tag = vlan_eth_h->h_vlan_TCI;
				} 
			}
			else {
				printk(KERN_DEBUG "%s:%d vlan eth header is NULL:\n", __func__, __LINE__);
			}
		}
		else {

			vlanh = skb_header_pointer(skb, 0, sizeof(_vlanh), &_vlanh);
			if(!vlanh)
				return -1;

			l2flow_temp->svlan_tag = vlanh->h_vlan_TCI;
			if (vlanh->h_vlan_encapsulated_proto == htons(ETH_P_8021Q)) {
				vlanh = skb_header_pointer(skb, sizeof(_vlanh), sizeof(_vlanh), &_vlanh);
				if(!vlanh) {
					printk("%s:%d VLAN HEADER NOT FOUND:\n", __func__, __LINE__);
					return -1;
				}
				l2flow_temp->cvlan_tag = vlanh->h_vlan_TCI;
			}
		} 
		return 0;
	}
	else if (ethertype == htons(ETH_P_PPP_SES)){
		struct pppoe_hdr *ph;
		struct pppoe_hdr _ph;
		
		ph = skb_header_pointer(skb, 0, sizeof(_ph), &_ph);
		if(!ph)
			return -1;

		l2flow_temp->session_id = ph->sid;

		return 0;
	}

	if(abm_l3_filtering){
		int l3_hdr_len;
		
		if(ethertype == htons(ETH_P_IP)){
			struct iphdr *iph;
			struct iphdr _iph;
			
			iph = skb_header_pointer(skb, 0, sizeof(_iph), &_iph);
			if(!iph)
				return -1;
			
			l2flow_temp->l3.saddr.ip = iph->saddr;
			l2flow_temp->l3.daddr.ip = iph->daddr;
			l2flow_temp->l3.proto = iph->protocol;
			l3_hdr_len = iph->ihl * 4;
			/* If Packet is fragmented, don't update L4 information */
			if(iph->frag_off & htons(IP_MF | IP_OFFSET))
				return 0;
		}
		else if (ethertype == htons(ETH_P_IPV6)){
			struct ipv6hdr *ip6h;
			struct ipv6hdr _ip6h;
			uint8_t nexthdr;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,6,0)
			__be16 frag_off;
#endif
			
			ip6h = skb_header_pointer(skb, 0, sizeof(_ip6h), &_ip6h);
			if(!ip6h)
				return -1;
			
			memcpy(l2flow_temp->l3.saddr.ip6, ip6h->saddr.s6_addr, 16);
			memcpy(l2flow_temp->l3.daddr.ip6, ip6h->daddr.s6_addr, 16);
			nexthdr = ip6h->nexthdr;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,6,0)
			l3_hdr_len = ipv6_skip_exthdr(skb, sizeof(_ip6h), &nexthdr, &frag_off);
#else
			l3_hdr_len = ipv6_skip_exthdr(skb, sizeof(_ip6h), &nexthdr);
#endif
			if(l3_hdr_len == -1)
				return -1;
			
			l2flow_temp->l3.proto = nexthdr;
		}
		else
			return -1; //We don't support

		if((l2flow_temp->l3.proto == IPPROTO_UDP) || (l2flow_temp->l3.proto == IPPROTO_TCP)){
			struct tcpudphdr *tcpudph;
			struct tcpudphdr _tcpudph;

			tcpudph = skb_header_pointer(skb, l3_hdr_len, sizeof(_tcpudph), &_tcpudph);
			if(!tcpudph)
				return -1;
			
			l2flow_temp->l4.sport = tcpudph->src;
			l2flow_temp->l4.dport = tcpudph->dst;
		}
	}
	return 0; //Success
}

/***************************************************************************
*
* abm_ebt_hook
* Core l2flow detection mechanism
*
****************************************************************************/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
static unsigned int abm_ebt_hook(void *priv,
			struct sk_buff *skb,
			const struct nf_hook_state *state)

#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,0,0)
static unsigned int abm_ebt_hook(const struct nf_hook_ops *ops,
			struct sk_buff *skb,
			const struct nf_hook_state *state)

#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3,13,0)
static unsigned int abm_ebt_hook(const struct nf_hook_ops *ops,
			struct sk_buff *skb,
			const struct net_device *in,
			const struct net_device *out,
			int (*okfn)(struct sk_buff *))
#else
static unsigned int abm_ebt_hook(unsigned int hooknum,
			struct sk_buff *skb,
			const struct net_device *in,
			const struct net_device *out,
			int (*okfn)(struct sk_buff *))
#endif
{
	struct l2flow l2flow_temp;
	struct l2flowTable *l2flow_entry;
	unsigned short ethertype;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
	unsigned int hooknum = state->hook;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3,13,0)
	unsigned int hooknum = ops->hooknum;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,0,0)
	const struct net_device* in = state->in;
	const struct net_device* out = state->out;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,0,0)
	if (skb_vlan_tag_present(skb))
#else
	if (vlan_tx_tag_present(skb))
#endif
		ethertype = htons(ETH_P_8021Q);
	else
		ethertype = eth_hdr(skb)->h_proto;

	if(!skb->abm_ff)
		goto exit0;
	
	if((ethertype != htons(ETH_P_IP))
	&& (ethertype != htons(ETH_P_IPV6))
	&& (ethertype != htons(ETH_P_PPP_SES))
	&& (ethertype != htons(ETH_P_8021Q))
	)
		goto exit0;

	memset(&l2flow_temp, 0, sizeof(l2flow_temp));
	if(abm_build_l2flow(skb, &l2flow_temp, ethertype) < 0)
		goto exit0;

	spin_lock(&abm_lock);

	if (hooknum == NF_BR_FORWARD) {
		if((l2flow_entry = abm_l2flow_find(&l2flow_temp)) == NULL){
			/* New entry */
			if((l2flow_entry = abm_l2flow_add(&l2flow_temp)) == NULL)
				goto exit1;
				
			l2flow_entry->state = L2FLOW_STATE_SEEN;
			l2flow_entry->idev_ifi = in->ifindex;
			mod_timer(&l2flow_entry->timeout, jiffies + l2flow_timeouts[l2flow_entry->state]);
		}
		else{
			if(in->ifindex != l2flow_entry->idev_ifi){
				l2flow_entry->flags |= L2FLOW_FL_NEEDS_UPDATE;
				l2flow_entry->idev_ifi = in->ifindex;
			}
		}
	}
	else if(hooknum == NF_BR_POST_ROUTING){
		if((l2flow_entry = abm_l2flow_find(&l2flow_temp)) != NULL){
			int rc;

			if(out->ifindex != l2flow_entry->odev_ifi){
				l2flow_entry->flags |= L2FLOW_FL_NEEDS_UPDATE;
				l2flow_entry->odev_ifi = out->ifindex;
			}
			l2flow_entry->packet_mark = skb->mark & 0xFFFF;

			switch(l2flow_entry->state)
			{
				case L2FLOW_STATE_SEEN:
					if((rc = abm_nl_send_l2flow_msg(abm_nl, L2FLOW_ENTRY_NEW, 0, l2flow_entry)) != -ENOTCONN){
						l2flow_entry->flags &= ~L2FLOW_FL_NEEDS_UPDATE;
						l2flow_entry->flags |= L2FLOW_FL_WAIT_ACK;
						l2flow_entry->time_sent = jiffies;
						list_add(&l2flow_entry->list_wait_for_ack, &l2flow_list_wait_for_ack);
					}
					l2flow_entry->state = L2FLOW_STATE_CONFIRMED;
					break;
				case L2FLOW_STATE_FF:
				case L2FLOW_STATE_LINUX:
					/* Updates are already handled via notifiers but we need this to update input interface in some cases*/
					/* However if we know that there is a pending message don't send it here */
					if(!(l2flow_entry->flags & L2FLOW_FL_PENDING_MSG) 
					&& (l2flow_entry->flags & L2FLOW_FL_NEEDS_UPDATE)){
						if((rc = abm_nl_send_l2flow_msg(abm_nl, L2FLOW_ENTRY_UPDATE, 0, l2flow_entry)) != -ENOTCONN){
							l2flow_entry->flags &= ~L2FLOW_FL_NEEDS_UPDATE;
							l2flow_entry->time_sent = jiffies;

							if(!(l2flow_entry->flags & L2FLOW_FL_WAIT_ACK)){
								list_add(&l2flow_entry->list_wait_for_ack, &l2flow_list_wait_for_ack);
								l2flow_entry->flags |= L2FLOW_FL_WAIT_ACK;
							}
						}
					}
					break;
				default:
					break;
			}//End switch

			if((l2flow_entry->state != L2FLOW_STATE_FF) && (l2flow_entry->state != L2FLOW_STATE_DYING)){
				mod_timer_pending(&l2flow_entry->timeout, jiffies + l2flow_timeouts[l2flow_entry->state]);
			}
		}
	}
exit1:
	spin_unlock(&abm_lock);
exit0:
	return NF_ACCEPT;
}

static struct nf_hook_ops abm_ebt_ops[] /*__read_mostly*/ = {
	{
		.hook		= abm_ebt_hook,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,4,0)
		.owner		= THIS_MODULE,
#endif
		.pf		= NFPROTO_BRIDGE,
		.hooknum	= NF_BR_FORWARD,
		.priority		= NF_BR_PRI_LAST,
	},
	{
		.hook		= abm_ebt_hook,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,4,0)
		.owner		= THIS_MODULE,
#endif
		.pf		= NFPROTO_BRIDGE,
		.hooknum	= NF_BR_POST_ROUTING,
		.priority		= NF_BR_PRI_LAST - 1,  //Just before bridge_netilter hook
	},
};

/***************************************************************************
*
* abm_l2flow_table_flush
* Flush l2flow table (called in user-context)
*
****************************************************************************/
static  void abm_l2flow_table_flush(void)
{
	int i;
	struct list_head *entry, *tmp;
	struct l2flowTable *table_entry;

	spin_lock_bh(&abm_lock);
	for(i = 0; i < L2FLOW_HASH_TABLE_SIZE; i++){
		list_for_each_safe(entry, tmp, &l2flow_table[i]){
			table_entry = container_of(entry, struct l2flowTable, list);
			table_entry->flags |= L2FLOW_FL_DEAD;
			if(del_timer(&table_entry->timeout) || table_entry->state == L2FLOW_STATE_FF)
				__abm_go_dying(table_entry);
		}
	}
	spin_unlock_bh(&abm_lock);
}

/***************************************************************************
*
* abm_l2flow_table_flush
* Small busy loop to wait for already expired timers 
*
****************************************************************************/
static  __inline void abm_l2flow_table_wait_timers(void)
{
	int i, empty;
test_list:
	empty = 1;
	for(i = 0; i < L2FLOW_HASH_TABLE_SIZE; i++)
		if(!list_empty(&l2flow_table[i])){
			empty = 0;
			break;
		}
		
	if(empty)
		return;
	else{
		schedule();
	}	goto test_list;
}

/***************************************************************************
*
* abm_l2flow_table_init
* Init l2flow table and l2flow cache
*
****************************************************************************/
static int abm_l2flow_table_init(void)
{
	int i;
	
	for(i = 0; i < L2FLOW_HASH_TABLE_SIZE; i++){
		INIT_LIST_HEAD(&l2flow_table[i]);
	}
	for(i = 0; i < L2FLOW_HASH_BY_MAC_TABLE_SIZE; i++){
		INIT_LIST_HEAD(&l2flow_table_by_src_mac[i]);
		INIT_LIST_HEAD(&l2flow_table_by_dst_mac[i]);
	}
	INIT_LIST_HEAD(&l2flow_list_msg_to_send);
	INIT_LIST_HEAD(&l2flow_list_wait_for_ack);
	
	l2flow_cache = kmem_cache_create("l2flow_cache",
					 sizeof(struct l2flowTable), 0, 0, NULL);
	if (!l2flow_cache)
		return -ENOMEM;

	return 0;
}

/***************************************************************************
*
* abm_l2flow_table_exit
* Flush all entries and de-allocate cache
*
****************************************************************************/
static void abm_l2flow_table_exit(void)
{
	abm_l2flow_table_flush();
	abm_l2flow_table_wait_timers();
	kmem_cache_destroy(l2flow_cache);
}

#ifdef CONFIG_PROC_FS
/***************************************************************************
*
*    Seq file implementation
*    Allow user to get L2 flow table via /proc/net/abm
*
****************************************************************************/

struct abm_seq_state{
	unsigned int bucket;
#ifdef CONFIG_NET_NS
	unsigned int tmp;
#endif
};

struct l2flowTable * abm_get_first(struct seq_file *seq)
{
	struct abm_seq_state *state = seq->private;
	int bucket = 0;

	while (bucket < L2FLOW_HASH_TABLE_SIZE){
		/* Return first entry present in a bucket */
		if(&l2flow_table[bucket] != l2flow_table[bucket].next){
			state->bucket = bucket;
			return container_of(l2flow_table[bucket].next, struct l2flowTable, list);
		}
		else
			bucket++;
	}
	return NULL; // Not found 
}

struct l2flowTable * abm_get_next(struct seq_file *seq, struct l2flowTable *table_entry)
{
	struct abm_seq_state *state = seq->private;
	struct list_head *entry;
	int bucket = state->bucket;

	entry = table_entry->list.next;

	if(entry != &l2flow_table[bucket]) // Next != Head
		return container_of(entry, struct l2flowTable, list);

	/* Move to next bucket */
	bucket++;

	/* Scan the hash-table and return the first entry linearly */
	while (bucket < L2FLOW_HASH_TABLE_SIZE){
		if(&l2flow_table[bucket] != l2flow_table[bucket].next){
			state->bucket = bucket;
			return container_of(l2flow_table[bucket].next, struct l2flowTable, list);
		}
		else
			bucket++;
	}
	return NULL; // Not found 
}

struct l2flowTable * abm_get_idx(struct seq_file *seq, loff_t *pos)
{
	loff_t idxpos = *pos; // > 1
	struct l2flowTable * entry = abm_get_first(seq);

	if(entry){
		idxpos--;
		while(idxpos){
			entry = abm_get_next(seq, entry);
			if(entry)
				idxpos--;
			else
				return NULL;
		}	
	}
	return entry;
}

void * abm_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	void *rc;
	
	if (v == SEQ_START_TOKEN) {
		rc = abm_get_first(seq);
	}
	else
		rc = abm_get_next(seq, v);

	++(*pos);
	return rc;
}

static int abm_seq_show(struct seq_file *seq, void *v)
{
	if (v == SEQ_START_TOKEN) {
		seq_puts(seq, "ABM L2 Flow entries dump\n------------------------\n");
	} else {
		struct l2flowTable* entry = (struct l2flowTable*)v;	
		struct l2flow *l2flowtmp = &entry->l2flow;

		seq_printf(seq, "  Saddr=%02x:%02x:%02x:%02x:%02x:%02x", l2flowtmp->saddr[0], l2flowtmp->saddr[1], l2flowtmp->saddr[2],
															l2flowtmp->saddr[3], l2flowtmp->saddr[4], l2flowtmp->saddr[5]);
		seq_printf(seq, "  Daddr=%02x:%02x:%02x:%02x:%02x:%02x", l2flowtmp->daddr[0], l2flowtmp->daddr[1], l2flowtmp->daddr[2],
															l2flowtmp->daddr[3], l2flowtmp->daddr[4], l2flowtmp->daddr[5]);
		seq_printf(seq, "  Ethertype=0x%04x", htons(l2flowtmp->ethertype));
		seq_printf(seq, "  Input itf=%d", entry->idev_ifi);
		seq_printf(seq, "  Output itf=%d", entry->odev_ifi);
		seq_printf(seq, "  Mark=0x%04x", entry->packet_mark);
		
		if(entry->l2flow.ethertype == htons(ETH_P_PPP_SES))
			seq_printf(seq, "  PPPoE Session id=%d", ntohs(l2flowtmp->session_id));
		else if(entry->l2flow.ethertype == htons(ETH_P_8021Q))
		{
			seq_printf(seq, "  SVLAN TCI=0x%04x", ntohs(l2flowtmp->svlan_tag));
			if(entry->l2flow.cvlan_tag)
				seq_printf(seq, "  CVLAN TCI=0x%04x", ntohs(l2flowtmp->cvlan_tag));
		}
		
		seq_printf(seq, "  State=[%s]", l2flow_states_string[entry->state]);
		
		if(entry->state != L2FLOW_STATE_FF)
			seq_printf(seq, "  Timeout=%ds",(int) (entry->timeout.expires- jiffies)/HZ);

		if(abm_l3_filtering){
			if(l2flowtmp->ethertype == htons(ETH_P_IP)){
				seq_printf(seq, " Src=%pI4", &l2flowtmp->l3.saddr.ip);
				seq_printf(seq, " Dst=%pI4", &l2flowtmp->l3.daddr.ip);
				seq_printf(seq, " Proto=%d", l2flowtmp->l3.proto);
			}
			else if (l2flowtmp->ethertype == htons(ETH_P_IPV6)){
				seq_printf(seq, " Src=%pI6", l2flowtmp->l3.saddr.ip6);
				seq_printf(seq, " Dst=%pI6", l2flowtmp->l3.daddr.ip6);
				seq_printf(seq, " Proto=%d", l2flowtmp->l3.proto);
			}
			if((l2flowtmp->l3.proto == IPPROTO_UDP) || (l2flowtmp->l3.proto == IPPROTO_TCP)){
				seq_printf(seq, " Sport=%d", ntohs(l2flowtmp->l4.sport));
				seq_printf(seq, " Dport=%d", ntohs(l2flowtmp->l4.dport));
			}
		}
		seq_printf(seq, "\n");
	}
	return 0;
}

static void *abm_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct abm_seq_state * state = seq->private;
	
	state->bucket = 0;
	spin_lock_bh(&abm_lock);
	
	return *pos ? abm_get_idx(seq, pos) : SEQ_START_TOKEN;
}

static void abm_seq_stop(struct seq_file *seq, void *v)
{
	spin_unlock_bh(&abm_lock);

	return;
}

static const struct seq_operations abm_seq_ops = {
	.start  = abm_seq_start,
	.next   = abm_seq_next,
	.stop   = abm_seq_stop,
	.show   = abm_seq_show,
};

static int abm_seq_open(struct inode *inode, struct file *file)
{
	return seq_open_net(inode, file, &abm_seq_ops,
			    sizeof(struct abm_seq_state));
}

static const struct file_operations abm_seq_fops = {
	.owner		= THIS_MODULE,
	.open		= abm_seq_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release		= seq_release_net,
};

static int __init abm_proc_init(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,9,0)
	if (!proc_create("abm", S_IRUGO, init_net.proc_net, &abm_seq_fops))
#else
	if (!proc_net_fops_create(&init_net, "abm", S_IRUGO, &abm_seq_fops))
#endif
		return -ENOMEM;
	return 0;
}

static void  abm_proc_fini(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,9,0)
	remove_proc_entry("abm", init_net.proc_net);
#else
	proc_net_remove(&init_net, "abm");
#endif
}

#else /* CONFIG_PROC_FS */

static int __init abm_proc_init(void)
{
	return 0;
}

static void  abm_proc_fini(void)
{

}

#endif /* CONFIG_PROC_FS */

#ifdef CONFIG_SYSCTL
/***************************************************************************
*
*    Sysctl implementation
*    Allow user to modify ABM parameters in /proc/sys/net/abm/...
*
****************************************************************************/

struct ctl_table_header * abm_sysctl_hdr;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,13,0)
static int abm_sysctl_l3_filtering(struct ctl_table *ctl, int write,
				  void __user *buffer,
				  size_t *lenp, loff_t *ppos)
#else
static int abm_sysctl_l3_filtering(ctl_table *ctl, int write,
				  void __user *buffer,
				  size_t *lenp, loff_t *ppos)
#endif
{
	int *valp = ctl->data;
	int val = *valp;
	int rc;
	int old_abm_l3_filtering = abm_l3_filtering;
	int ret = proc_dointvec(ctl, write, buffer, lenp, ppos);

	if (write && *valp != val) {
		if(((!old_abm_l3_filtering) && *valp) || (old_abm_l3_filtering && (!*valp))){
			abm_l2flow_table_flush();
			
			if((rc = abm_nl_send_rst_msg(abm_nl)) < 0)
				ABM_PRINT(KERN_ERR, " Netlink send rst msg error = %d\n", rc);
			
		}
		abm_l3_filtering = (*valp) ? 1 : 0;
	}
	return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0)
struct ctl_path abm_sysctl_path[] = {
	{ .procname = "net", },
	{ .procname = "abm", },
	{ }
};
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,13,0)
static struct ctl_table abm_sysctl_table[] = 
#else
static ctl_table abm_sysctl_table[] = 
#endif
{
	{
		.procname	= "abm_l3_filtering",
		.data		= &abm_l3_filtering,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= abm_sysctl_l3_filtering,
	},
	{
		.procname	= "abm_timeout_seen",
		.data		= &l2flow_timeouts[L2FLOW_STATE_SEEN],
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_jiffies,
	},
	{
		.procname	= "abm_timeout_confirmed",
		.data		= &l2flow_timeouts[L2FLOW_STATE_CONFIRMED],
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_jiffies,
	},
	{
		.procname	= "abm_timeout_linux",
		.data		= &l2flow_timeouts[L2FLOW_STATE_LINUX],
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_jiffies,
	},
	{
		.procname	= "abm_timeout_dying",
		.data		= &l2flow_timeouts[L2FLOW_STATE_DYING],
		.maxlen 	= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_jiffies,
	},
	{
		.procname	= "abm_retransmit_delay",
		.data		= &abm_retransmit_time,
		.maxlen 		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_jiffies,
	},
	{
		.procname	= "abm_max_entries",
		.data		= &abm_max_entries,
		.maxlen 		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{ }
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,13,0)
static int __net_init abm_net_init(struct net *net)
{
	struct ctl_table *table;

	table = abm_sysctl_table;

	abm_sysctl_hdr = register_net_sysctl(net, "net/abm", table);
	if (!abm_sysctl_hdr)
	{
		printk(KERN_ERR "%s():: Auto bridge module sysctl init failed:\n", __func__);
	}

	return 0;
}

static void __net_exit abm_net_exit(struct net *net)
{
	if (abm_sysctl_hdr == NULL)
		return;

	unregister_net_sysctl_table(abm_sysctl_hdr);
}

static struct pernet_operations __net_initdata abm_net_ops = {
	.init	= abm_net_init,
	.exit	= abm_net_exit,
};
#endif

static int __init abm_sysctl_init(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,13,0)
	register_pernet_subsys(&abm_net_ops);
#else
	if((abm_sysctl_hdr = register_net_sysctl_table(&init_net, abm_sysctl_path,
						abm_sysctl_table)) == NULL)
		return -1;
#endif

	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0)
static void abm_sysctl_fini(void)
{
	unregister_net_sysctl_table(abm_sysctl_hdr);
}
#endif

#else
static int __init abm_sysctl_init(void)
{
	return 0;
}

static void abm_sysctl_fini(void)
{

}

#endif

/***************************************************************************
*
*   abm_init 
*   Module initialisation
*
****************************************************************************/
static int abm_init(void)
{
	int rc = 0;

	printk(KERN_DEBUG "Initializing Automatic bridging module v%s\n", auto_bridge_version);
	if((kabm_wq = create_singlethread_workqueue("abm_wq")) == NULL){
		rc = -ENOMEM;
		ABM_PRINT(KERN_ERR, "Automatic bridging module error creating wq rc = %d \n", rc);
		return rc;
	}
	if((rc = abm_l2flow_table_init()) < 0){
		ABM_PRINT(KERN_ERR, "Automatic bridging module error l2flow_table init rc = %d \n", rc);
		return rc;
	}
	br_fdb_register_can_expire_cb(&abm_fdb_can_expire);
	if((rc = abm_nl_init()) < 0){
		ABM_PRINT(KERN_ERR, "Automatic bridging module error netlink init int rc = %d \n", rc);
		return rc;
	}
	if((rc = abm_proc_init()) < 0){
		ABM_PRINT(KERN_ERR, "Automatic bridging module error can't create /proc file rc = %d \n", rc);
		return rc;
	}
	if((rc = abm_sysctl_init()) < 0){
		ABM_PRINT(KERN_ERR, "Automatic bridging module error can't create sysctl rc = %d \n", rc);
		return rc;
	}
	if((rc = nf_register_net_hooks(&init_net,abm_ebt_ops, ARRAY_SIZE(abm_ebt_ops))) < 0){
	ABM_PRINT(KERN_ERR, "Automatic bridging module error can't register hooks int rc = %d \n", rc);
		return rc;
	}
	register_brevent_notifier(&abm_br_notifier);
	queue_delayed_work(kabm_wq, &abm_work_retransmit, abm_retransmit_time);
	
	return 0;
}

/***************************************************************************
*
*   abm_exit 
*   Module exit, can't fail
*
****************************************************************************/
static void abm_exit(void)
{
	printk(KERN_DEBUG "Exiting Automatic bridging module \n");
	unregister_brevent_notifier(&abm_br_notifier);
	cancel_work_sync(&abm_work_send_msg);
	cancel_delayed_work_sync(&abm_work_retransmit);
	destroy_workqueue(kabm_wq);
	nf_unregister_net_hooks(&init_net,abm_ebt_ops, ARRAY_SIZE(abm_ebt_ops));
	abm_nl_exit();
	br_fdb_deregister_can_expire_cb();
	abm_l2flow_table_exit();
	abm_proc_fini();
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0)
	abm_sysctl_fini();
#endif
}


module_init(abm_init);
module_exit(abm_exit);

