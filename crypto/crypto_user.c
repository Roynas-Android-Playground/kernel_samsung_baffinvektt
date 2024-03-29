/*
 * Crypto user configuration API.
 *
 * Copyright (C) 2011 secunet Security Networks AG
 * Copyright (C) 2011 Steffen Klassert <steffen.klassert@secunet.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/module.h>
#include <linux/crypto.h>
#include <linux/cryptouser.h>
#include <net/netlink.h>
#include <linux/security.h>
#include <net/net_namespace.h>
#include "internal.h"

DEFINE_MUTEX(crypto_cfg_mutex);

/* The crypto netlink socket */
static struct sock *crypto_nlsk;

struct crypto_dump_info {
	struct sk_buff *in_skb;
	struct sk_buff *out_skb;
	u32 nlmsg_seq;
	u16 nlmsg_flags;
};

static struct crypto_alg *crypto_alg_match(struct crypto_user_alg *p, int exact)
{
	int match;
	struct crypto_alg *q, *alg = NULL;

	down_read(&crypto_alg_sem);

	if (list_empty(&crypto_alg_list))
		return NULL;

	list_for_each_entry(q, &crypto_alg_list, cra_list) {

		if ((q->cra_flags ^ p->cru_type) & p->cru_mask)
			continue;

		if (strlen(p->cru_driver_name))
			match = !strcmp(q->cra_driver_name,
					p->cru_driver_name);
		else if (!exact)
			match = !strcmp(q->cra_name, p->cru_name);

		if (match) {
			alg = q;
			break;
		}
	}

	up_read(&crypto_alg_sem);

	return alg;
}

static int crypto_report_one(struct crypto_alg *alg,
			     struct crypto_user_alg *ualg, struct sk_buff *skb)
{
	memcpy(&ualg->cru_name, &alg->cra_name, sizeof(ualg->cru_name));
	memcpy(&ualg->cru_driver_name, &alg->cra_driver_name,
	       sizeof(ualg->cru_driver_name));
	memcpy(&ualg->cru_module_name, module_name(alg->cra_module),
	       CRYPTO_MAX_ALG_NAME);

	ualg->cru_flags = alg->cra_flags;
	ualg->cru_refcnt = atomic_read(&alg->cra_refcnt);

	NLA_PUT_U32(skb, CRYPTOCFGA_PRIORITY_VAL, alg->cra_priority);

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

static int crypto_report_alg(struct crypto_alg *alg,
			     struct crypto_dump_info *info)
{
	struct sk_buff *in_skb = info->in_skb;
	struct sk_buff *skb = info->out_skb;
	struct nlmsghdr *nlh;
	struct crypto_user_alg *ualg;
	int err = 0;

	nlh = nlmsg_put(skb, NETLINK_CB(in_skb).pid, info->nlmsg_seq,
			CRYPTO_MSG_GETALG, sizeof(*ualg), info->nlmsg_flags);
	if (!nlh) {
		err = -EMSGSIZE;
		goto out;
	}

	ualg = nlmsg_data(nlh);

	err = crypto_report_one(alg, ualg, skb);
	if (err) {
		nlmsg_cancel(skb, nlh);
		goto out;
	}

	nlmsg_end(skb, nlh);

out:
	return err;
}

static int crypto_report(struct sk_buff *in_skb, struct nlmsghdr *in_nlh,
			 struct nlattr **attrs)
{
	struct crypto_user_alg *p = nlmsg_data(in_nlh);
	struct crypto_alg *alg;
	struct sk_buff *skb;
	struct crypto_dump_info info;
	int err;

	if (!p->cru_driver_name)
		return -EINVAL;

	alg = crypto_alg_match(p, 1);
	if (!alg)
		return -ENOENT;

	skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	info.in_skb = in_skb;
	info.out_skb = skb;
	info.nlmsg_seq = in_nlh->nlmsg_seq;
	info.nlmsg_flags = 0;

	err = crypto_report_alg(alg, &info);
	if (err)
		return err;

	return nlmsg_unicast(crypto_nlsk, skb, NETLINK_CB(in_skb).pid);
}

static int crypto_dump_report(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct crypto_alg *alg;
	struct crypto_dump_info info;
	int err;

	if (cb->args[0])
		goto out;

	cb->args[0] = 1;

	info.in_skb = cb->skb;
	info.out_skb = skb;
	info.nlmsg_seq = cb->nlh->nlmsg_seq;
	info.nlmsg_flags = NLM_F_MULTI;

	list_for_each_entry(alg, &crypto_alg_list, cra_list) {
		err = crypto_report_alg(alg, &info);
		if (err)
			goto out_err;
	}

out:
	return skb->len;
out_err:
	return err;
}

static int crypto_dump_report_done(struct netlink_callback *cb)
{
	return 0;
}

static int crypto_update_alg(struct sk_buff *skb, struct nlmsghdr *nlh,
			     struct nlattr **attrs)
{
	struct crypto_alg *alg;
	struct crypto_user_alg *p = nlmsg_data(nlh);
	struct nlattr *priority = attrs[CRYPTOCFGA_PRIORITY_VAL];
	LIST_HEAD(list);

	if (priority && !strlen(p->cru_driver_name))
		return -EINVAL;

	alg = crypto_alg_match(p, 1);
	if (!alg)
		return -ENOENT;

	down_write(&crypto_alg_sem);

	crypto_remove_spawns(alg, &list, NULL);

	if (priority)
		alg->cra_priority = nla_get_u32(priority);

	up_write(&crypto_alg_sem);

	crypto_remove_final(&list);

	return 0;
}

static int crypto_del_alg(struct sk_buff *skb, struct nlmsghdr *nlh,
			  struct nlattr **attrs)
{
	struct crypto_alg *alg;
	struct crypto_user_alg *p = nlmsg_data(nlh);

	alg = crypto_alg_match(p, 1);
	if (!alg)
		return -ENOENT;

	/* We can not unregister core algorithms such as aes-generic.
	 * We would loose the reference in the crypto_alg_list to this algorithm
	 * if we try to unregister. Unregistering such an algorithm without
	 * removing the module is not possible, so we restrict to crypto
	 * instances that are build from templates. */
	if (!(alg->cra_flags & CRYPTO_ALG_INSTANCE))
		return -EINVAL;

	if (atomic_read(&alg->cra_refcnt) != 1)
		return -EBUSY;

	return crypto_unregister_instance(alg);
}

static int crypto_add_alg(struct sk_buff *skb, struct nlmsghdr *nlh,
			  struct nlattr **attrs)
{
	int exact;
	const char *name;
	struct crypto_alg *alg;
	struct crypto_user_alg *p = nlmsg_data(nlh);
	struct nlattr *priority = attrs[CRYPTOCFGA_PRIORITY_VAL];

	if (strlen(p->cru_driver_name))
		exact = 1;

	if (priority && !exact)
		return -EINVAL;

	alg = crypto_alg_match(p, exact);
	if (alg)
		return -EEXIST;

	if (strlen(p->cru_driver_name))
		name = p->cru_driver_name;
	else
		name = p->cru_name;

	alg = crypto_alg_mod_lookup(name, p->cru_type, p->cru_mask);
	if (IS_ERR(alg))
		return PTR_ERR(alg);

	down_write(&crypto_alg_sem);

	if (priority)
		alg->cra_priority = nla_get_u32(priority);

	up_write(&crypto_alg_sem);

	crypto_mod_put(alg);

	return 0;
}

#define MSGSIZE(type) sizeof(struct type)

static const int crypto_msg_min[CRYPTO_NR_MSGTYPES] = {
	[CRYPTO_MSG_NEWALG	- CRYPTO_MSG_BASE] = MSGSIZE(crypto_user_alg),
	[CRYPTO_MSG_DELALG	- CRYPTO_MSG_BASE] = MSGSIZE(crypto_user_alg),
	[CRYPTO_MSG_UPDATEALG	- CRYPTO_MSG_BASE] = MSGSIZE(crypto_user_alg),
	[CRYPTO_MSG_GETALG	- CRYPTO_MSG_BASE] = MSGSIZE(crypto_user_alg),
};

static const struct nla_policy crypto_policy[CRYPTOCFGA_MAX+1] = {
	[CRYPTOCFGA_PRIORITY_VAL]   = { .type = NLA_U32},
};

#undef MSGSIZE

static struct crypto_link {
	int (*doit)(struct sk_buff *, struct nlmsghdr *, struct nlattr **);
	int (*dump)(struct sk_buff *, struct netlink_callback *);
	int (*done)(struct netlink_callback *);
} crypto_dispatch[CRYPTO_NR_MSGTYPES] = {
	[CRYPTO_MSG_NEWALG	- CRYPTO_MSG_BASE] = { .doit = crypto_add_alg},
	[CRYPTO_MSG_DELALG	- CRYPTO_MSG_BASE] = { .doit = crypto_del_alg},
	[CRYPTO_MSG_UPDATEALG	- CRYPTO_MSG_BASE] = { .doit = crypto_update_alg},
	[CRYPTO_MSG_GETALG	- CRYPTO_MSG_BASE] = { .doit = crypto_report,
						       .dump = crypto_dump_report,
						       .done = crypto_dump_report_done},
};

static int crypto_user_rcv_msg(struct sk_buff *skb, struct nlmsghdr *nlh)
{
	struct nlattr *attrs[CRYPTOCFGA_MAX+1];
	struct crypto_link *link;
	int type, err;

	type = nlh->nlmsg_type;
	if (type > CRYPTO_MSG_MAX)
		return -EINVAL;

	type -= CRYPTO_MSG_BASE;
	link = &crypto_dispatch[type];

	if ((type == (CRYPTO_MSG_GETALG - CRYPTO_MSG_BASE) &&
	    (nlh->nlmsg_flags & NLM_F_DUMP))) {
		if (link->dump == NULL)
			return -EINVAL;

		return netlink_dump_start(crypto_nlsk, skb, nlh,
					  link->dump, link->done, 0);
	}

	err = nlmsg_parse(nlh, crypto_msg_min[type], attrs, CRYPTOCFGA_MAX,
			  crypto_policy);
	if (err < 0)
		return err;

	if (link->doit == NULL)
		return -EINVAL;

	return link->doit(skb, nlh, attrs);
}

static void crypto_netlink_rcv(struct sk_buff *skb)
{
	mutex_lock(&crypto_cfg_mutex);
	netlink_rcv_skb(skb, &crypto_user_rcv_msg);
	mutex_unlock(&crypto_cfg_mutex);
}

static int __init crypto_user_init(void)
{
	crypto_nlsk = netlink_kernel_create(&init_net, NETLINK_CRYPTO,
					    0, crypto_netlink_rcv,
					    NULL, THIS_MODULE);
	if (!crypto_nlsk)
		return -ENOMEM;

	return 0;
}

static void __exit crypto_user_exit(void)
{
	netlink_kernel_release(crypto_nlsk);
}

module_init(crypto_user_init);
module_exit(crypto_user_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Steffen Klassert <steffen.klassert@secunet.com>");
MODULE_DESCRIPTION("Crypto userspace configuration API");
