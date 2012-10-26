#include <linux/module.h>#include <linux/kernel.h>#include <linux/string.h>#include <linux/delay.h>#include <linux/errno.h>#include <linux/interrupt.h>#include <linux/init.h>#include <linux/netdevice.h>#include <linux/etherdevice.h>#include <linux/skbuff.h>#include <linux/wakelock.h>#include <linux/workqueue.h>#include <linux/ipv6.h>#include <linux/ip.h>#include <asm/byteorder.h>#include "xmd-ch.h"#ifdef CONFIG_HAS_EARLYSUSPEND#include <linux/earlysuspend.h>#endif#define POLL_DELAY 1000000 #define RMNET_ERR#define RMNET_WD_TMO		5 * HZ#define RMNET_DATAT_SIZE   	ETH_DATA_LEN #define RMNET_MTU_SIZE		( ETH_HLEN + RMNET_DATAT_SIZE )#define MAX_PART_PKT_SIZE   2500#define RMNET_ARP_ENABLE#if defined (RMNET_CHANGE_MTU)#define RMNET_MIN_MTU		64#define RMNET_MAX_MTU		4096#endiftypedef enum {	RMNET_FULL_PACKET,	RMNET_PARTIAL_PACKET,	RMNET_PARTIAL_HEADER,} RMNET_PAST_STATE;static struct {	RMNET_PAST_STATE state;	char buf[MAX_PART_PKT_SIZE];	int size;	int type;} past_packet[MAX_SMD_NET];static struct xmd_ch_info rmnet_channels[MAX_SMD_NET] = {	{0,  "CHANNEL13",  0, XMD_NET, NULL, 0, SPIN_LOCK_UNLOCKED},	{1,  "CHANNEL14",  0, XMD_NET, NULL, 0, SPIN_LOCK_UNLOCKED},	{2,  "CHANNEL15",  0, XMD_NET, NULL, 0, SPIN_LOCK_UNLOCKED},};struct rmnet_private {	struct xmd_ch_info *ch;	struct net_device_stats stats;	const char *chname;	struct wake_lock wake_lock;	struct ethhdr eth_hdr;	int ip_type;#ifdef CONFIG_MSM_RMNET_DEBUG	ktime_t last_packet;	short active_countdown; 	short restart_count; 	unsigned long wakeups_xmit;	unsigned long wakeups_rcv;	unsigned long timeout_us;	unsigned long awake_time_ms;	struct delayed_work work;#endif};static struct {	int blocked;	struct net_device *dev;} rmnet_ch_block_info[16];#if defined (RMNET_ARP_ENABLE)struct arp_resp {	__be16          ar_hrd;         	__be16          ar_pro;         	unsigned char   ar_hln;         	unsigned char   ar_pln;         	__be16          ar_op;          	unsigned char           ar_sha[ETH_ALEN];       	unsigned char           ar_sip[4];              	unsigned char           ar_tha[ETH_ALEN];       	unsigned char           ar_tip[4];              };#endif#if defined (RMNET_ERR)#define RMNET_COL_SIZE 30#define RMNET_ERROR_STR_SIZE 20static unsigned int rmnet_cnt = 0;#endifstatic void xmd_net_dump(const unsigned char *txt, const unsigned char *buf, int len){	char dump_buf_str[(RMNET_COL_SIZE+1)*3] = {0,};	int index = 0;	char ch = 0;	char* cur_str = dump_buf_str;	if ((buf != NULL) && (len >= 0))	{		while(index < RMNET_COL_SIZE)		{			if(index < len)			{				ch = buf[index];				sprintf(cur_str, "x%.2x", ch);			}			else			{				sprintf(cur_str, "$$$");			}			cur_str = cur_str+3;			index++;		}		*cur_str = 0;		printk("\n%s: rmnet_cnt [%d]th : len [%d] buf [%s]\n", txt, rmnet_cnt, len, dump_buf_str);		rmnet_cnt++;	}}static int count_this_packet(void *_hdr, int len){	struct ethhdr *hdr = _hdr;	if (len >= ETH_HLEN && hdr->h_proto == htons(ETH_P_ARP)) {		return 0;	}	return 1;}#ifdef CONFIG_MSM_RMNET_DEBUGstatic int in_suspend;static unsigned long timeout_us;static struct workqueue_struct *rmnet_wq;static void do_check_active(struct work_struct *work){	struct rmnet_private *p =		container_of(work, struct rmnet_private, work.work);	if (in_suspend) {		int tmp = p->timeout_us * 2 -			(p->timeout_us / (p->active_countdown + 1));		tmp /= 1000;		p->awake_time_ms += tmp;		p->active_countdown = p->restart_count = 0;		return;	}	p->restart_count++;	if (--p->active_countdown == 0) {		p->awake_time_ms += p->restart_count * POLL_DELAY / 1000;		p->restart_count = 0;	} else {		queue_delayed_work(rmnet_wq, &p->work,				usecs_to_jiffies(POLL_DELAY));	}}#ifdef CONFIG_HAS_EARLYSUSPENDstatic unsigned long timeout_suspend_us;static struct device *rmnet0;static ssize_t timeout_suspend_store(	struct device *d,	struct device_attribute *attr,	const char *buf, size_t n){	timeout_suspend_us = simple_strtoul(buf, NULL, 10);	return n;}static ssize_t timeout_suspend_show(	struct device *d,	struct device_attribute *attr,	char *buf){	return sprintf(buf, "%lu\n", (unsigned long) timeout_suspend_us);}static DEVICE_ATTR (timeout_suspend,					0664,					timeout_suspend_show,					timeout_suspend_store);static void rmnet_early_suspend(struct early_suspend *handler){	if (rmnet0) {		struct rmnet_private *p = netdev_priv(to_net_dev(rmnet0));		p->timeout_us = timeout_suspend_us;	}	in_suspend = 1;}static void rmnet_late_resume(struct early_suspend *handler){	if (rmnet0) {		struct rmnet_private *p = netdev_priv(to_net_dev(rmnet0));		p->timeout_us = timeout_us;	}	in_suspend = 0;}static struct early_suspend rmnet_power_suspend = {	.suspend = rmnet_early_suspend,	.resume = rmnet_late_resume,};static int __init rmnet_late_init(void){	register_early_suspend(&rmnet_power_suspend);	return 0;}late_initcall(rmnet_late_init);#endifstatic int rmnet_cause_wakeup(struct rmnet_private *p){	int ret = 0;	ktime_t now;	if (p->timeout_us == 0) 		return 0;	if (p->active_countdown == 0) {		ret = 1;		now = ktime_get_real();		p->last_packet = now;		if (in_suspend) {			queue_delayed_work(rmnet_wq, &p->work,					usecs_to_jiffies(p->timeout_us));		} else {			queue_delayed_work(rmnet_wq, &p->work,					usecs_to_jiffies(POLL_DELAY));		}	}	if (in_suspend) {		p->active_countdown++;	} else {		p->active_countdown = p->timeout_us / POLL_DELAY;	}	return ret;}static ssize_t wakeups_xmit_show(struct device *d,	struct device_attribute *attr,	char *buf){	struct rmnet_private *p = netdev_priv(to_net_dev(d));	return sprintf(buf, "%lu\n", p->wakeups_xmit);}DEVICE_ATTR(wakeups_xmit, 0444, wakeups_xmit_show, NULL);static ssize_t wakeups_rcv_show(	struct device *d,	struct device_attribute *attr,	char *buf){	struct rmnet_private *p = netdev_priv(to_net_dev(d));	return sprintf(buf, "%lu\n", p->wakeups_rcv);}DEVICE_ATTR(wakeups_rcv, 0444, wakeups_rcv_show, NULL);static ssize_t timeout_store(	struct device *d,	struct device_attribute *attr,	const char *buf, size_t n){#ifndef CONFIG_HAS_EARLYSUSPEND	struct rmnet_private *p = netdev_priv(to_net_dev(d));	p->timeout_us = timeout_us = simple_strtoul(buf, NULL, 10);#else	timeout_us = simple_strtoul(buf, NULL, 10);#endif	return n;}static ssize_t timeout_show(	struct device *d,	struct device_attribute *attr,	char *buf){	struct rmnet_private *p = netdev_priv(to_net_dev(d));	p = netdev_priv(to_net_dev(d));	return sprintf(buf, "%lu\n", timeout_us);}DEVICE_ATTR(timeout, 0664, timeout_show, timeout_store);static ssize_t awake_time_show(	struct device *d,	struct device_attribute *attr,	char *buf){	struct rmnet_private *p = netdev_priv(to_net_dev(d));	return sprintf(buf, "%lu\n", p->awake_time_ms);}DEVICE_ATTR(awake_time_ms, 0444, awake_time_show, NULL);#endif#define RMNET_ARP_VER 0x2#define RMNET_IPV6_VER 0x6#define RMNET_IPV4_VER 0x4static int xmd_trans_packet(	struct net_device *dev,	int type,	void *buf,	int sz){	struct rmnet_private *p = netdev_priv(dev);	struct sk_buff *skb;	void *ptr = NULL;	sz += ETH_HLEN; #if defined (RMNET_CRITICAL_DEBUG)	printk("\nRMNET: %d<\n",sz);#endif	if ((type != RMNET_IPV4_VER) && (type != RMNET_IPV6_VER )#if defined (RMNET_ARP_ENABLE)		&& (type != RMNET_ARP_VER )#endif				) {#if defined (RMNET_ERR)		printk("\n%s (line %d) invalid type(%x)\n", __func__, __LINE__, type);#endif		p->stats.rx_errors++;		return -EINVAL;	}#if defined (RMNET_CHANGE_MTU)	if (sz > dev->mtu)#else	if (sz > RMNET_MTU_SIZE)#endif	{#if defined (RMNET_ERR)		printk("\n%s (line %d) discarding pkt len (%d) version %d\n", __func__, __LINE__, sz, type);#endif		p->stats.rx_errors++;		return -EINVAL;	}	else {		skb = dev_alloc_skb(sz + NET_IP_ALIGN);		if (skb == NULL) {#if defined (RMNET_ERR)			printk("\n%s (line %d) cannot allocate dev_alloc_skb type(%x) pkt len (%d) \n",				__func__, __LINE__, type, sz);#endif			p->stats.rx_dropped++;			return -ENOMEM;		}		else {			skb->dev = dev;			skb_reserve(skb, NET_IP_ALIGN);			ptr = skb_put(skb, sz);			wake_lock_timeout(&p->wake_lock, HZ / 2);#if 0			{				char temp[] = {0xB6,0x91,0x24,0xa8,0x14,0x72,0xb6,0x91,0x24,0xa8,0x14,0x72,0x08,0x0};				struct ethhdr *eth_hdr = (struct ethhdr *) temp;				if (type == RMNET_IPV6_VER) {					eth_hdr->h_proto = htons(ETH_P_IPV6);				}#if defined (RMNET_ARP_ENABLE)				else if (type == RMNET_ARP_VER) {					eth_hdr->h_proto = htons(ETH_P_ARP);				}#endif								else 				{					eth_hdr->h_proto = htons(ETH_P_IP);				}				memcpy((void *)eth_hdr->h_dest,					   (void*)dev->dev_addr,					   sizeof(eth_hdr->h_dest));				memcpy((void *)ptr,					   (void *)eth_hdr,					   sizeof(struct ethhdr));			}#else			if (type != p->ip_type)			{				if (type == RMNET_IPV6_VER) {					p->eth_hdr.h_proto = htons(ETH_P_IPV6);				}#if defined (RMNET_ARP_ENABLE)								else if (type == RMNET_ARP_VER) {					p->eth_hdr.h_proto = htons(ETH_P_ARP);				}#endif				else 				{					p->eth_hdr.h_proto = htons(ETH_P_IP);				}				p->ip_type = type;			}			memcpy((void *)ptr, (void *)&p->eth_hdr, ETH_HLEN);#endif			memcpy(ptr + ETH_HLEN, buf, sz - ETH_HLEN);			skb->protocol = eth_type_trans(skb, dev);			if (count_this_packet(ptr, skb->len)) {#ifdef CONFIG_MSM_RMNET_DEBUG				p->wakeups_rcv += rmnet_cause_wakeup(p);#endif				p->stats.rx_packets++;				p->stats.rx_bytes += skb->len;			}			netif_rx(skb);			wake_unlock(&p->wake_lock);		}	}	return 0;}static void rmnet_reset_pastpacket_info(int ch){	if(ch >= MAX_SMD_NET ) {#if defined (RMNET_DEBUG)		printk("\nrmnet:Invalid rmnet channel number %d\n", ch);#endif		return;	}		past_packet[ch].state = RMNET_FULL_PACKET;	memset(past_packet[ch].buf, 0 , MAX_PART_PKT_SIZE);	past_packet[ch].size = 0;	past_packet[ch].type = 0;}static void xmd_net_notify(int chno){	int i, rc = 0;	struct net_device *dev = NULL;	void *buf = NULL;	int tot_sz = 0;	struct rmnet_private *p = NULL;	struct xmd_ch_info *info = NULL;	for (i=0; i<ARRAY_SIZE(rmnet_channels); i++) {		if (rmnet_channels[i].chno == chno)	{			dev = (struct net_device *)rmnet_channels[i].priv;			break;		}	}	if (!dev) {#if defined (RMNET_ERR)		printk("\n%s (line %d) No device\n", __func__, __LINE__);#endif		return;	}	p = netdev_priv(dev);	if (!p) {#if defined (RMNET_ERR)		printk("\n%s (line %d) No netdev_priv\n", __func__, __LINE__);#endif		return;	}	info = p->ch;	if (!info) {#if defined (RMNET_ERR)		printk("\n%s (line %d) No info\n", __func__, __LINE__);#endif		return;	}	buf = xmd_ch_read(info->chno, &tot_sz);	if (!buf) {#if defined (RMNET_ERR)		printk("\n%s (line %d) No buf recvd from ch(%d)\n", __func__ ,__LINE__, info->chno);#endif		return;	}#if defined (RMNET_DEBUG)	printk("\nxmd_net_notify: total size read = %d from ch:%d \n",			tot_sz, info->chno);#endif	switch (past_packet[info->id].state)	{	case RMNET_FULL_PACKET:	break;	case RMNET_PARTIAL_PACKET:	{		void *ip_hdr = (void *)past_packet[info->id].buf;		int sz;		int copy_size;#if defined (RMNET_DEBUG)		printk("\nxmd_net_notify: past partial packet\n");#endif		if (past_packet[info->id].type == RMNET_IPV4_VER) {			sz = ntohs(((struct iphdr*) ip_hdr)->tot_len);		} else if (past_packet[info->id].type == RMNET_IPV6_VER) {			sz = ntohs(((struct ipv6hdr*) ip_hdr)->payload_len) + sizeof(struct ipv6hdr);		} else {#if defined (RMNET_ERR)			printk("\n%s (line %d) Invalid past version(%d)\n", __func__ ,__LINE__ 				,past_packet[info->id].type);#endif			rmnet_reset_pastpacket_info(info->id);			p->stats.rx_errors++;			return;		}		if(sz > RMNET_DATAT_SIZE) {#if defined (RMNET_ERR)			printk("\n%s (line %d) Invalid hdr len(%d)\n", __func__, __LINE__, sz);			xmd_net_dump("xmd_net_notify", past_packet[info->id].buf, past_packet[info->id].size);#endif			rmnet_reset_pastpacket_info(info->id);			return;		}		copy_size = sz - past_packet[info->id].size;		 		if (tot_sz >= copy_size) {			memcpy(past_packet[info->id].buf + past_packet[info->id].size,buf,copy_size);		} else {			memcpy(past_packet[info->id].buf + past_packet[info->id].size,buf,tot_sz);#if defined (RMNET_DEBUG)			printk("\nxmd_net_notify: RMNET_PARTIAL_PACKET. past size = %d, total size = %d\n",						past_packet[info->id].size, tot_sz);#endif			past_packet[info->id].size += tot_sz;			return;		}		rc = xmd_trans_packet(dev,past_packet[info->id].type,(void*)past_packet[info->id].buf,sz);		if(rc != 0) {#if defined (RMNET_ERR)			printk("\n%s (line %d) xmd_trans_packet fail sz(%d) tot_sz(%d)\n",				__func__, __LINE__, sz, tot_sz);			xmd_net_dump("xmd_net_notify", past_packet[info->id].buf, sz);#endif			rmnet_reset_pastpacket_info(info->id);			return;		}#if defined (RMNET_DEBUG)		printk("xmd_net_notify: pushed reassembled data packet to tcpip, sz = %d\n", sz);#endif		buf = buf + copy_size;		tot_sz = tot_sz - copy_size;		past_packet[info->id].state = RMNET_FULL_PACKET;	}	break;	case RMNET_PARTIAL_HEADER:	{		void *ip_hdr = (void *)past_packet[info->id].buf;		int sz;		int copy_size;		int hdr_size = 0;#if defined (RMNET_DEBUG)		printk("xmd_net_notify: past partial header packet\n");#endif		if (past_packet[info->id].type == RMNET_IPV4_VER)			hdr_size = sizeof(struct iphdr);		else if (past_packet[info->id].type  == RMNET_IPV6_VER)			hdr_size = sizeof(struct ipv6hdr);		else		{#if defined (RMNET_ERR)			printk("\n%s (line %d) Invalid past version (%d)\n", __func__, __LINE__,				past_packet[info->id].type);#endif			rmnet_reset_pastpacket_info(info->id);			p->stats.rx_errors++;			return;		}		copy_size = hdr_size - past_packet[info->id].size;		if(tot_sz >= copy_size) {			memcpy(past_packet[info->id].buf + past_packet[info->id].size,buf,copy_size);		} else {			memcpy(past_packet[info->id].buf + past_packet[info->id].size,buf,tot_sz);#if defined (RMNET_DEBUG)			printk("\n%s (line %d) Still partial header\n", __func__, __LINE__);#endif			past_packet[info->id].size += tot_sz;			return;		}		buf = buf + copy_size;		tot_sz = tot_sz - copy_size;		past_packet[info->id].size = past_packet[info->id].size + copy_size;		if (past_packet[info->id].type == RMNET_IPV4_VER) {			sz = ntohs(((struct iphdr*) ip_hdr)->tot_len);		} else if (past_packet[info->id].type == RMNET_IPV6_VER) {			sz = ntohs(((struct ipv6hdr*) ip_hdr)->payload_len) + sizeof(struct ipv6hdr);		} else {#if defined (RMNET_ERR)			printk("\n%s (line %d) Invalid past version (%d)\n", __func__, __LINE__,							past_packet[info->id].type);#endif			rmnet_reset_pastpacket_info(info->id);			p->stats.rx_errors++;			return;		}		if(sz > RMNET_DATAT_SIZE) {#if defined (RMNET_ERR)			printk("\n%s (line %d) Invalid hdr len(%d)\n", __func__, __LINE__, sz);			xmd_net_dump("xmd_net_notify", past_packet[info->id].buf, past_packet[info->id].size);#endif			rmnet_reset_pastpacket_info(info->id);			return;		}		copy_size = sz - past_packet[info->id].size;		 		if (tot_sz >= copy_size) {			memcpy(past_packet[info->id].buf + past_packet[info->id].size,buf,copy_size);		} else {			memcpy(past_packet[info->id].buf + past_packet[info->id].size,buf,tot_sz);#if defined (RMNET_DEBUG)			printk("\n%s (line %d) past size = %d, total size = %d\n",  __func__, __LINE__,						past_packet[info->id].size, tot_sz);#endif			past_packet[info->id].size += tot_sz;			past_packet[info->id].state = RMNET_PARTIAL_PACKET;			return;		}		rc = xmd_trans_packet(dev,past_packet[info->id].type,(void *)past_packet[info->id].buf,sz);		if(rc != 0) {#if defined (RMNET_ERR)			printk("\n%s (line %d) xmd_trans_packet fail sz(%d) tot_sz(%d)\n",				__func__, __LINE__, sz, tot_sz);			xmd_net_dump("xmd_net_notify", past_packet[info->id].buf, sz);#endif			rmnet_reset_pastpacket_info(info->id);			return;		}		buf = buf + copy_size;		tot_sz = tot_sz - copy_size;	}	break;	default:	{#if defined (RMNET_ERR)		printk("\n%s (line %d) Invalid past state (%d)\n", __func__, __LINE__, 				(int)past_packet[info->id].state);#endif		rmnet_reset_pastpacket_info(info->id);		p->stats.rx_errors++;		return;	}	break;	}	while (tot_sz > 0) {		int hdr_size = 0;		int ver = 0;		void *ip_hdr = (void *)buf;		int data_sz = 0;#if defined(__BIG_ENDIAN_BITFIELD)		ver = ((char *)buf)[0] & 0x0F;#elif defined(__LITTLE_ENDIAN_BITFIELD)		ver = (((char *)buf)[0] & 0xF0) >> 4;#endif#if defined (RMNET_DEBUG)		printk("xmd_net_notify: ver = 0x%x, total size : %d \n", ver, tot_sz);#endif		if (ver == RMNET_IPV4_VER) {			hdr_size = sizeof(struct iphdr);		} else if (ver == RMNET_IPV6_VER) {			hdr_size = sizeof(struct ipv6hdr);		} else {#if 1			if(NULL != strnstr((char*)buf, "+PBREADY", 12)) {#if defined (RMNET_ERR)				printk("xmd_net_notify: +PBREADY Left packet size= %d\n", tot_sz);#endif				rmnet_reset_pastpacket_info(info->id);#if 0				ifx_schedule_cp_dump_or_reset();#endif			}			else {				#if defined (RMNET_ERR)								char buf_str[RMNET_ERROR_STR_SIZE];				memset(buf_str, 0x00, RMNET_ERROR_STR_SIZE);				memcpy(buf_str, buf, min(RMNET_ERROR_STR_SIZE, tot_sz));				printk("xmd_net_notify: ch:%d Invalid version, 0x%x\n", info->chno, ver);				printk("xmd_net_notify: Few bytes of pkt : 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",					((char *)buf)[0], ((char *)buf)[1], ((char *)buf)[2],					((char *)buf)[3], ((char *)buf)[4], ((char *)buf)[5],					((char *)buf)[6], ((char *)buf)[7], ((char *)buf)[8],					((char *)buf)[9], ((char *)buf)[10]);				printk("xmd_net_notify: Converted strings %s\n", buf_str);				printk("xmd_net_notify: Left packet size = %d, \n", tot_sz);#endif				rmnet_reset_pastpacket_info(info->id);			}#endif		return;		}		if (tot_sz < hdr_size) {			past_packet[info->id].state = RMNET_PARTIAL_HEADER;			past_packet[info->id].size = tot_sz;			memcpy(past_packet[info->id].buf, buf, tot_sz);			past_packet[info->id].type = ver;#if defined (RMNET_DEBUG)			printk("\n%s (line %d) partial header packet copied locally, sz = %d\n",					__func__, __LINE__,	tot_sz);#endif			return;		}		if (ver == RMNET_IPV4_VER) {			data_sz = ntohs(((struct iphdr*) ip_hdr)->tot_len);		} else if (ver == RMNET_IPV6_VER) {			data_sz = ntohs(((struct ipv6hdr*) ip_hdr)->payload_len) + sizeof(struct ipv6hdr);		} else {#if defined (RMNET_ERR)			printk("\n%s (line %d) data sz check -- Invalid version, %d\n", __func__, __LINE__, ver);#endif			rmnet_reset_pastpacket_info(info->id);			p->stats.rx_errors++;			break;		}		if(data_sz > RMNET_DATAT_SIZE) {#if defined (RMNET_ERR)			printk("\n%s (line %d) Invalid hdr len(%d)\n", __func__, __LINE__, data_sz);			xmd_net_dump("xmd_net_notify", buf, tot_sz);#endif			rmnet_reset_pastpacket_info(info->id);			return;		}#if defined (RMNET_DEBUG)		printk("xmd_net_notify: data size = %d\n", data_sz);#endif		if (tot_sz < data_sz) {			past_packet[info->id].state = RMNET_PARTIAL_PACKET;			past_packet[info->id].size = tot_sz;			memcpy(past_packet[info->id].buf, buf, tot_sz);			past_packet[info->id].type = ver;#if defined (RMNET_DEBUG)			printk("\n%s (line %d) partial data packet copied locally, sz = %d\n",				__func__, __LINE__,	tot_sz);#endif			return;		}		rc = xmd_trans_packet(dev, ver, buf, data_sz);		if(rc != 0) {#if defined (RMNET_ERR)			printk("\n%s (line %d) xmd_trans_packet fail data_sz(%d) tot_sz(%d)\n",				__func__, __LINE__, data_sz, tot_sz);			xmd_net_dump("xmd_net_notify", buf, data_sz);#endif			rmnet_reset_pastpacket_info(info->id);			return;		}#if defined (RMNET_DEBUG)		printk("xmd_net_notify: pushed full data packet to tcpip, sz = %d\n",				data_sz);#endif		tot_sz = tot_sz - data_sz;		buf = buf + data_sz;#if defined (RMNET_DEBUG)		printk("xmd_net_notify: looping for another packet tot_sz = %d\n",				tot_sz);#endif	}	past_packet[info->id].state = RMNET_FULL_PACKET;}static int rmnet_open(struct net_device *dev){	struct rmnet_private *p = netdev_priv(dev);	struct xmd_ch_info *info = NULL;	if (!p) {#if defined (RMNET_ERR)		printk("\n%s (line %d) No netdev_priv\n", __func__, __LINE__);#endif		return -ENODEV;	}	info = p->ch;	if (!info) {#if defined (RMNET_ERR)		printk("\n%s (line %d) No xmd_ch_info\n", __func__, __LINE__);#endif		return -ENODEV;	}#if 0	if (!netif_carrier_ok(dev)) {		netif_carrier_on(dev);	}#endif	info->chno = xmd_ch_open(info, xmd_net_notify);	if (info->chno < 0) {#if defined (RMNET_ERR)		printk("\n%s (line %d) error ch (%d)\n", __func__, __LINE__, info->chno);#endif		info->chno = 0;		return -ENODEV;	}	printk("rmnet_open: ch %d \n", info->chno);	netif_start_queue(dev);	return 0;}static int rmnet_stop(struct net_device *dev){	struct rmnet_private *p = netdev_priv(dev);	struct xmd_ch_info *info = NULL;	if (!p) {#if defined (RMNET_ERR)		printk("\n%s (line %d) No netdev_priv\n", __func__, __LINE__);#endif		return -ENODEV;	}	info = p->ch;	if (!info) {#if defined (RMNET_ERR)		printk("\n%s (line %d) No xmd_ch_info\n", __func__, __LINE__);#endif		return -ENODEV;	}	printk("rmnet_stop() ch %d \n", info->chno);	netif_stop_queue(dev);	xmd_ch_close(info->chno);	rmnet_reset_pastpacket_info(info->id);	rmnet_ch_block_info[info->chno].dev = NULL;	rmnet_ch_block_info[info->chno].blocked = 0;	return 0;}void rmnet_restart_queue(int chno){	if(rmnet_ch_block_info[chno].blocked) {		rmnet_ch_block_info[chno].blocked = 0;		netif_wake_queue(rmnet_ch_block_info[chno].dev);#if defined (RMNET_DEBUG)		printk("rmnet: FIFO free so unblocking rmnet %d queue\n", chno);#endif	}}#if defined (RMNET_ARP_ENABLE)void fake_arp_response(struct net_device *dev, struct sk_buff *skb){	struct arp_resp *arp_resp, *arp_req;	char macAddr[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};	arp_resp = (struct arp_resp *) kmalloc(sizeof(struct arp_resp), GFP_ATOMIC);	if(arp_resp == NULL)	{#if defined (RMNET_ERR)		printk("\n%s (line %d) cannot allocate\n", __func__, __LINE__);#endif		return;	}	memset(arp_resp, sizeof(struct arp_resp), 0);	arp_req = (struct arp_resp *) (skb->data + ETH_HLEN);	arp_resp->ar_hrd = arp_req->ar_hrd;	arp_resp->ar_pro = arp_req->ar_pro;	arp_resp->ar_hln = arp_req->ar_hln;	arp_resp->ar_pln = arp_req->ar_pln;	arp_resp->ar_op = htons(2);	memcpy((void *)arp_resp->ar_sha, (void *)macAddr, ETH_HLEN);	memcpy((void *)arp_resp->ar_sip,(void *)arp_req->ar_tip , 4);	memcpy((void *)arp_resp->ar_tha,(void *)arp_req->ar_sha, ETH_HLEN);	memcpy((void *)arp_resp->ar_tip,(void *)arp_req->ar_sip, 4);	xmd_trans_packet(dev, RMNET_ARP_VER, (void *)arp_resp, sizeof(struct arp_resp));	kfree(arp_resp);}#endifstatic int rmnet_xmit(struct sk_buff *skb, struct net_device *dev){	struct rmnet_private *p = netdev_priv(dev);	struct xmd_ch_info *info = NULL;	int ret = NETDEV_TX_OK;	if (!p) {#if defined (RMNET_ERR)		printk("\n%s (line %d) No netdev_priv\n", __func__, __LINE__);#endif		goto ok_xmit;	}	info = p->ch;	if (!info) {#if defined (RMNET_ERR)		printk("\n%s (line %d) No xmd_ch_info\n", __func__, __LINE__);#endif		goto ok_xmit;	}#if defined (RMNET_CRITICAL_DEBUG)	printk("\nRMNET[%d]: %d>\n", info->chno, skb->len);#endif	if((skb->len - ETH_HLEN) <= 0) {#if defined (RMNET_ERR)		printk("\n%s (line %d) Got only header for ch (%d)\n", __func__, __LINE__, info->chno);#endif		goto ok_xmit;	}#if 0 	if ((skb->protocol != htons(ETH_P_IP)) && (skb->protocol != htons(ETH_P_IPV6))#if defined (RMNET_ARP_ENABLE)				&& (skb->protocol != htons(ETH_P_ARP))#endif				) {		xmd_net_dump("rmnet_xmit", (char*)skb->data, skb->len);	}#endif#if 0 	if ((skb->data[ETH_HLEN] != 0x45) && (skb->data[ETH_HLEN] != 0x60)) {		xmd_net_dump("rmnet_xmit", (char*)skb->data, skb->len);	}#endif	#if defined (RMNET_ARP_ENABLE)	if (skb->protocol == htons(ETH_P_ARP)) {		fake_arp_response(dev, skb);		goto ok_xmit;	}#endif	if ((ret = xmd_ch_write(info->chno,(void *)((char *) skb->data + ETH_HLEN), skb->len - ETH_HLEN)) != 0) {		p->stats.tx_errors++;		if(ret == -ENOMEM) {			netif_stop_queue(dev);			rmnet_ch_block_info[info->chno].dev = dev;			rmnet_ch_block_info[info->chno].blocked = 1;			ret = NETDEV_TX_BUSY;#if defined (RMNET_ERR)			printk("\n%s (line %d) Cannot alloc mem, so returning busy for ch (%d)\n", __func__, __LINE__, info->chno);#endif			goto quit_xmit;		} else if(ret == -EBUSY) {			netif_stop_queue(dev);			rmnet_ch_block_info[info->chno].dev = dev;			rmnet_ch_block_info[info->chno].blocked = 1;#if defined (RMNET_ERR)			printk("\n%s (line %d) Stopping queue for ch (%d)\n", __func__, __LINE__, info->chno);#endif			ret = NETDEV_TX_BUSY;			goto quit_xmit;		} else if (ret == -ENOTBLK) {			netif_stop_queue(dev);#if 0			netif_carrier_off(dev);#endif#if defined (RMNET_ERR)			printk("\n%s (line %d) ENOTBLK for ch (%d)\n", __func__, __LINE__, info->chno);#endif		}		else {#if defined (RMNET_ERR)			printk("\n%s (line %d) etc error for ch (%d)\n", __func__, __LINE__, info->chno);#endif		}				} else {		if (count_this_packet(skb->data, skb->len)) {			p->stats.tx_packets++;			p->stats.tx_bytes += skb->len;#ifdef CONFIG_MSM_RMNET_DEBUG			p->wakeups_xmit += rmnet_cause_wakeup(p);#endif		}	}ok_xmit:	ret = NETDEV_TX_OK;	dev_kfree_skb_irq(skb);quit_xmit:	return ret;}static struct net_device_stats *rmnet_get_stats(struct net_device *dev){	struct rmnet_private *p = netdev_priv(dev);	if (!p) {#if defined (RMNET_ERR)		printk("\n%s (line %d) No netdev_priv\n", __func__, __LINE__);#endif        return NULL;	}	return &p->stats;}static void rmnet_set_multicast_list(struct net_device *dev){	struct rmnet_private *p = netdev_priv(dev);	struct xmd_ch_info *info = NULL;	if (!p) {#if defined (RMNET_ERR)		printk("\n%s (line %d) No netdev_priv\n", __func__, __LINE__);#endif		return;	}	info = p->ch;	if (!info) {#if defined (RMNET_ERR)		printk("rmnet_set_multicast_list: No xmd_ch_info \n");#endif		return;	}	printk("rmnet_set_multicast_list ch %d \n", info->chno);}static void rmnet_tx_timeout(struct net_device *dev){	int chno;	for(chno=13; chno < 16; chno++) {		if(rmnet_ch_block_info[chno].dev == dev) {#if 0			rmnet_restart_queue(chno);#endif			printk("rmnet_tx_timeout()ch %d \n", chno);			break;		}	}}#if defined (RMNET_CHANGE_MTU)static int rmnet_nd_change_mtu(struct net_device *dev, int new_mtu){	struct rmnet_private *p = netdev_priv(dev);	struct xmd_ch_info *info = NULL;	if (!p) {#if defined (RMNET_ERR)		printk("\n%s (line %d) No netdev_priv\n", __func__, __LINE__);#endif		return -EINVAL;	}	info = p->ch;	if (!info) {#if defined (RMNET_ERR)		printk("rmnet_nd_change_mtu: No xmd_ch_info \n");#endif		return -EINVAL;	}	printk("rmnet_nd_change_mtu ch %d new_mtu [%d]\n", info->chno, new_mtu);	if (new_mtu < RMNET_MIN_MTU || new_mtu > RMNET_MAX_MTU)		return -EINVAL;	p->mtu = new_mtu;	return 0;}#endifstatic struct net_device_ops rmnet_ops = {	.ndo_open = rmnet_open,	.ndo_stop = rmnet_stop,	.ndo_start_xmit = rmnet_xmit,	.ndo_get_stats = rmnet_get_stats,	.ndo_set_multicast_list = rmnet_set_multicast_list,	.ndo_tx_timeout = rmnet_tx_timeout,#if defined (RMNET_CHANGE_MTU)	.ndo_change_mtu = rmnet_nd_change_mtu,#endif};static void __init rmnet_setup(struct net_device *dev){	dev->netdev_ops = &rmnet_ops;	dev->watchdog_timeo = RMNET_WD_TMO;	ether_setup(dev);	dev->mtu = RMNET_MTU_SIZE;#if !defined (RMNET_ARP_ENABLE)	dev->flags |= IFF_NOARP;#endif	random_ether_addr(dev->dev_addr);}static void rmnet_set_ip4_ethr_hdr(struct net_device *dev, struct rmnet_private *p){	unsigned char faddr[ETH_ALEN] = { 0xb6, 0x91, 0x24, 0xa8, 0x14, 0x72 };	memcpy((void *)p->eth_hdr.h_dest,		(void *)dev->dev_addr, ETH_ALEN);	memcpy((void *)p->eth_hdr.h_source, 		(void *)faddr, ETH_ALEN);	p->eth_hdr.h_proto = htons(ETH_P_IP);	p->ip_type = RMNET_IPV4_VER;}static int __init rmnet_init(void){	int ret;	struct net_device *dev;	struct rmnet_private *p;	unsigned n;#ifdef CONFIG_MSM_RMNET_DEBUG	struct device *d;	timeout_us = 0;#ifdef CONFIG_HAS_EARLYSUSPEND	timeout_suspend_us = 0;#endif#endif#ifdef CONFIG_MSM_RMNET_DEBUG	rmnet_wq = create_workqueue("rmnet");#endif	for (n = 0; n < MAX_SMD_NET; n++) {		rmnet_reset_pastpacket_info(n);		dev = alloc_netdev(sizeof(struct rmnet_private),				   "rmnet%d", rmnet_setup);		if (!dev) {#if defined (RMNET_ERR)			printk("rmnet_init: alloc_netdev fail \n");#endif				return -ENOMEM;		}#ifdef CONFIG_MSM_RMNET_DEBUG		d = &(dev->dev);#endif		p = netdev_priv(dev);		if (!p) {#if defined (RMNET_ERR)			printk("rmnet_init: No netdev_priv \n");#endif		}		rmnet_channels[n].priv = (void *)dev;		p->ch = rmnet_channels + n;		p->chname = rmnet_channels[n].name;		wake_lock_init(&p->wake_lock,						WAKE_LOCK_SUSPEND,						rmnet_channels[n].name);		rmnet_set_ip4_ethr_hdr(dev, p);#ifdef CONFIG_MSM_RMNET_DEBUG		p->timeout_us = timeout_us;		p->awake_time_ms = p->wakeups_xmit = p->wakeups_rcv = 0;		p->active_countdown = p->restart_count = 0;		INIT_DELAYED_WORK_DEFERRABLE(&p->work, do_check_active);#endif		ret = register_netdev(dev);		if (ret) {#if defined (RMNET_ERR)			printk("rmnet_init: register_netdev fail \n");#endif						free_netdev(dev);			return ret;		}#ifdef CONFIG_MSM_RMNET_DEBUG		if (device_create_file(d, &dev_attr_timeout))			continue;		if (device_create_file(d, &dev_attr_wakeups_xmit))			continue;		if (device_create_file(d, &dev_attr_wakeups_rcv))			continue;		if (device_create_file(d, &dev_attr_awake_time_ms))			continue;#ifdef CONFIG_HAS_EARLYSUSPEND		if (device_create_file(d, &dev_attr_timeout_suspend))			continue;		if (n == 0)			rmnet0 = d;#endif#endif	}	return 0;}module_init(rmnet_init);