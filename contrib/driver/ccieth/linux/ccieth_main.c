/*
 * CCI over Ethernet
 * Copyright © INRIA 2011
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>

#include <ccieth_io.h>
#include <ccieth_common.h>
#include <ccieth_wire.h>

struct idr ccieth_ep_idr;
static spinlock_t ccieth_ep_idr_lock;

static int ccieth_destroy_connection_idrforeach_cb(int id, void *p, void *data)
{
	struct ccieth_connection *conn = p;
	kfree(conn);
	return 0;
}

static void
ccieth_destroy_endpoint(struct ccieth_endpoint *ep)
{
	struct ccieth_endpoint_event *event, *nevent;

	spin_lock(&ccieth_ep_idr_lock);
	idr_remove(&ccieth_ep_idr, ep->id);
	spin_unlock(&ccieth_ep_idr_lock);
	dev_put(ep->ifp);
	list_for_each_entry_safe(event, nevent, &ep->event_list, list) {
		list_del(&event->list);
		kfree(event);
	}
	idr_for_each(&ep->connection_idr, ccieth_destroy_connection_idrforeach_cb, NULL);
	idr_remove_all(&ep->connection_idr);
	kfree(ep);
}

static struct ccieth_endpoint *
ccieth_create_endpoint(struct ccieth_ioctl_create_endpoint *arg)
{
	struct ccieth_endpoint *ep;
	struct net_device *ifp;
	int id;
	int err;

	rcu_read_lock();
	ifp = dev_getbyhwaddr_rcu(&init_net, ARPHRD_ETHER, (const char *)&arg->addr);
	if (!ifp) /* allow loopback to ease development */
		ifp = dev_getbyhwaddr_rcu(&init_net, ARPHRD_LOOPBACK, (const char *)&arg->addr);
	if (!ifp) {
		rcu_read_unlock();
		err = -ENODEV;
		goto out;
	}
	dev_hold(ifp);
	rcu_read_unlock();

	ep = kmalloc(sizeof(struct ccieth_endpoint), GFP_KERNEL);
	if (!ep) {
		err = -ENOMEM;
		goto out_with_ifp;
	}
	ep->ifp = ifp;

	if (!idr_pre_get(&ccieth_ep_idr, GFP_KERNEL)) {
		err = -ENOMEM;
		goto out_with_ep;
	}
	spin_lock(&ccieth_ep_idr_lock);
	err = idr_get_new(&ccieth_ep_idr, ep, &id);
	spin_unlock(&ccieth_ep_idr_lock);
	if (err)
		goto out_with_ep;

	INIT_LIST_HEAD(&ep->event_list);
	spin_lock_init(&ep->event_list_lock);

	idr_init(&ep->connection_idr);
	spin_lock_init(&ep->connection_idr_lock);

	arg->id = ep->id = id;

	return ep;

out_with_ep:
	kfree(ep);
out_with_ifp:
	dev_put(ifp);
out:
	return ERR_PTR(err);
}

static int
ccieth_get_event(struct ccieth_endpoint *ep, struct ccieth_ioctl_get_event *arg)
{
	struct ccieth_endpoint_event *event;

	spin_lock(&ep->event_list_lock);
	if (list_empty(&ep->event_list)) {
		spin_unlock(&ep->event_list_lock);
		return -EAGAIN;
	}

	event = list_first_entry(&ep->event_list, struct ccieth_endpoint_event, list);
	list_del(&event->list);
	spin_unlock(&ep->event_list_lock);

	memcpy(arg, &event->event, sizeof(*arg));
	kfree(event);
	return 0;
}

static int
ccieth_return_event(struct ccieth_endpoint *ep, const struct ccieth_ioctl_return_event *arg)
{
	/* FIXME: nothing to do for now */
	return 0;
}

static int
ccieth_send_connect(struct ccieth_endpoint *ep, const struct ccieth_ioctl_send_connect *arg)
{
	struct sk_buff *skb;
	struct ccieth_pkt_header *hdr;
	int err;

        skb = alloc_skb(ETH_ZLEN, GFP_KERNEL);
	if (!skb) {
		err = -ENOMEM;
		goto out;
	}
		
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb->protocol = __constant_htons(ETH_P_CCI);
	skb_put(skb, ETH_ZLEN);
	skb->dev = ep->ifp;

	hdr = (struct ccieth_pkt_header *) skb_mac_header(skb);
	memcpy(&hdr->eth.h_dest, &arg->dest_addr, 6);
	memset(&hdr->eth.h_dest, 0x12, 6);
	memcpy(&hdr->eth.h_source, ep->ifp->dev_addr, 6);
	hdr->eth.h_proto = __constant_cpu_to_be16(ETH_P_CCI);
	hdr->endpoint_id = arg->dest_eid;
	hdr->type = CCIETH_PKT_CONNECT;
        dev_queue_xmit(skb);
	return 0;

out:
	return err;
}

static int
ccieth_miscdev_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static int
ccieth_miscdev_release(struct inode *inode, struct file *file)
{
	struct ccieth_endpoint *ep = file->private_data;
	if (ep) {
		file->private_data = NULL;
		ccieth_destroy_endpoint(ep);
	}
	return 0;
}

static long
ccieth_miscdev_ioctl(struct file *file, unsigned cmd, unsigned long arg)
{
	int ret;

	switch (cmd) {
	case CCIETH_IOCTL_GET_INFO: {
		/* get a sockaddr_ll from userspace */
		struct ccieth_ioctl_get_info gi_arg;
		struct net_device *ifp;

		ret = copy_from_user(&gi_arg, (const __user void *)arg, sizeof(gi_arg));
		if (ret)
			return -EFAULT;

		gi_arg.max_send_size = -1;
		gi_arg.pci_domain = -1;
		gi_arg.pci_bus = -1;
		gi_arg.pci_dev = -1;
		gi_arg.pci_func = -1;
		gi_arg.rate = 0;

		rcu_read_lock();
		ifp = dev_getbyhwaddr_rcu(&init_net, ARPHRD_ETHER, (const char *)&gi_arg.addr);
		if (ifp) {
			struct device *dev = ifp->dev.parent;

			if (ifp->mtu == 9000)
				gi_arg.max_send_size = 8192;
			else if (ifp->mtu == 1500)
				gi_arg.max_send_size = 1024;

			if (dev && dev->bus == &pci_bus_type) {
				struct pci_dev *pdev = to_pci_dev(dev);
				gi_arg.pci_domain = pci_domain_nr(pdev->bus);
				gi_arg.pci_bus = pdev->bus->number;
				gi_arg.pci_dev = PCI_SLOT(pdev->devfn);
				gi_arg.pci_func = PCI_FUNC(pdev->devfn);
			}

			if (ifp->ethtool_ops && ifp->ethtool_ops->get_settings) {
				struct ethtool_cmd cmd;
				if (ifp->ethtool_ops->get_settings(ifp, &cmd) >= 0) {
					u32 speed = ethtool_cmd_speed(&cmd);
					if (speed != -1)
						gi_arg.rate = ((u64) speed) * 1000000;
				}
			}
		} else
			ret = -ENODEV;
		rcu_read_unlock();

		ret = copy_to_user((__user void *)arg, &gi_arg, sizeof(gi_arg));
		if (ret)
			return -EFAULT;


		return 0;
	}

	case CCIETH_IOCTL_CREATE_ENDPOINT: {
		struct ccieth_ioctl_create_endpoint ce_arg;
		struct ccieth_endpoint *ep, **epp;

		ret = copy_from_user(&ce_arg, (const __user void *)arg, sizeof(ce_arg));
		if (ret)
			return -EFAULT;

		ep = ccieth_create_endpoint(&ce_arg);
		if (IS_ERR(ep))
			return PTR_ERR(ep);

		epp = (struct ccieth_endpoint **)&file->private_data;
		if (cmpxchg(epp, NULL, ep)) {
			ccieth_destroy_endpoint(ep);
			return -EBUSY;
		}

		ret = copy_to_user((__user void *)arg, &ce_arg, sizeof(ce_arg));
		if (ret)
			return -EFAULT;

		return 0;
	}

	case CCIETH_IOCTL_GET_EVENT: {
		struct ccieth_ioctl_get_event ge_arg;
		struct ccieth_endpoint *ep = file->private_data;

		if (!ep)
			return -EINVAL;

		ret = ccieth_get_event(ep, &ge_arg);
		if (ret < 0)
			return ret;

		/* FIXME: copy directly from the event list to user-space */
		ret = copy_to_user((__user void *)arg, &ge_arg, sizeof(ge_arg));
		if (ret)
			return -EFAULT;

		return 0;
	}

	case CCIETH_IOCTL_RETURN_EVENT: {
		struct ccieth_ioctl_return_event re_arg;
		struct ccieth_endpoint *ep = file->private_data;

		if (!ep)
			return -EINVAL;

		ret = copy_from_user(&re_arg, (__user void *)arg, sizeof(re_arg));
		if (ret)
			return -EFAULT;

		ret = ccieth_return_event(ep, &re_arg);
		if (ret < 0)
			return ret;

		return 0;
	}

	case CCIETH_IOCTL_SEND_CONNECT: {
		struct ccieth_ioctl_send_connect sc_arg;
		struct ccieth_endpoint *ep = file->private_data;

		if (!ep)
			return -EINVAL;

		ret = copy_from_user(&sc_arg, (__user void *)arg, sizeof(sc_arg));
		if (ret)
			return -EFAULT;

		ret = ccieth_send_connect(ep, &sc_arg);
		if (ret < 0)
			return ret;

		return 0;
	}

	default:
		return -EINVAL;
	}
}

static struct file_operations
ccieth_miscdev_fops = {
	.owner = THIS_MODULE,
	.open = ccieth_miscdev_open,
	.release = ccieth_miscdev_release,
	.unlocked_ioctl = ccieth_miscdev_ioctl,
};

static struct miscdevice
ccieth_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ccieth",
	.fops = &ccieth_miscdev_fops,
};

int
ccieth_init(void)
{
	int ret;

	idr_init(&ccieth_ep_idr);
	spin_lock_init(&ccieth_ep_idr_lock);

	ret = misc_register(&ccieth_miscdev);
	if (ret < 0)
		goto out;

	ccieth_recv_init();

	return 0;

out:
	return ret;
}

void
ccieth_exit(void)
{
	ccieth_recv_exit();
	misc_deregister(&ccieth_miscdev);
	idr_destroy(&ccieth_ep_idr);
}

module_init(ccieth_init);
module_exit(ccieth_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Brice Goglin <Brice.Goglin@inria.fr>");
MODULE_VERSION("0.0.1");
MODULE_DESCRIPTION("CCI over Ethernet");
