/*
 * Copyright (C) 2014 Felix Fietkau <nbd@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "mt76.h"

static int
mt76_start(struct ieee80211_hw *hw)
{
	struct mt76_dev *dev = hw->priv;
	int ret;

	mutex_lock(&dev->mutex);

	ret = mt76_mac_start(dev);
	if (ret)
		goto out;

	ret = mt76_phy_start(dev);
	if (ret)
		goto out;

	ieee80211_queue_delayed_work(dev->hw, &dev->mac_work,
				     MT_CALIBRATE_INTERVAL);
	napi_enable(&dev->napi);

	set_bit(MT76_STATE_RUNNING, &dev->state);

out:
	mutex_unlock(&dev->mutex);
	return ret;
}

static void
mt76_stop(struct ieee80211_hw *hw)
{
	struct mt76_dev *dev = hw->priv;

	mutex_lock(&dev->mutex);
	napi_disable(&dev->napi);
	clear_bit(MT76_STATE_RUNNING, &dev->state);
	mt76_stop_hardware(dev);
	mutex_unlock(&dev->mutex);
}

static int
mt76_add_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct mt76_dev *dev = hw->priv;
	struct mt76_vif *mvif = (struct mt76_vif *) vif->drv_priv;
	unsigned int idx = 0;
	int ret = 0;

	if (vif->addr[0] & BIT(1))
		idx = 1 + (((dev->macaddr[0] ^ vif->addr[0]) >> 2) & 7);

	/*
	 * Client mode typically only has one configurable BSSID register,
	 * which is used for bssidx=0. This is linked to the MAC address.
	 * Since mac80211 allows changing interface types, and we cannot
	 * force the use of the primary MAC address for a station mode
	 * interface, we need some other way of configuring a per-interface
	 * remote BSSID.
	 * The hardware provides an AP-Client feature, where bssidx 0-7 are
	 * used for AP mode and bssidx 8-15 for client mode.
	 * We shift the station interface bss index by 8 to force the
	 * hardware to recognize the BSSID.
	 * The resulting bssidx mismatch for unicast frames is ignored by hw.
	 */
	if (vif->type == NL80211_IFTYPE_STATION)
		idx += 8;

	mvif->idx = idx;
	mvif->group_wcid.idx = 254 - idx;
	mvif->group_wcid.hw_key_idx = -1;
	mt76_txq_init(dev, vif->txq);

	return ret;
}

static void
mt76_remove_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct mt76_dev *dev = hw->priv;

	mt76_txq_remove(dev, vif->txq);
}

static int
mt76_config(struct ieee80211_hw *hw, u32 changed)
{
	struct mt76_dev *dev = hw->priv;
	int ret = 0;

	mutex_lock(&dev->mutex);

	if (changed & IEEE80211_CONF_CHANGE_POWER) {
		dev->txpower_conf = hw->conf.power_level;

		if (test_bit(MT76_STATE_RUNNING, &dev->state))
			mt76_phy_set_txpower(dev);
	}

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		ieee80211_stop_queues(hw);
		ret = mt76_set_channel(dev, &hw->conf.chandef);
		ieee80211_wake_queues(hw);
	}

	mutex_unlock(&dev->mutex);

	return ret;
}

static void
mt76_configure_filter(struct ieee80211_hw *hw, unsigned int changed_flags,
		      unsigned int *total_flags, u64 multicast)
{
	struct mt76_dev *dev = hw->priv;
	u32 flags = 0;

#define MT76_FILTER(_flag, _hw) do { \
		flags |= *total_flags & FIF_##_flag;			\
		dev->rxfilter &= ~(_hw);				\
		dev->rxfilter |= !(flags & FIF_##_flag) * (_hw);	\
	} while (0)

	mutex_lock(&dev->mutex);

	dev->rxfilter &= ~MT_RX_FILTR_CFG_OTHER_BSS;

	MT76_FILTER(PROMISC_IN_BSS, MT_RX_FILTR_CFG_PROMISC);
	MT76_FILTER(FCSFAIL, MT_RX_FILTR_CFG_CRC_ERR);
	MT76_FILTER(PLCPFAIL, MT_RX_FILTR_CFG_PHY_ERR);
	MT76_FILTER(CONTROL, MT_RX_FILTR_CFG_ACK |
			     MT_RX_FILTR_CFG_CTS |
			     MT_RX_FILTR_CFG_CFEND |
			     MT_RX_FILTR_CFG_CFACK |
			     MT_RX_FILTR_CFG_BA |
			     MT_RX_FILTR_CFG_CTRL_RSV);
	MT76_FILTER(PSPOLL, MT_RX_FILTR_CFG_PSPOLL);

	*total_flags = flags;
	mt76_wr(dev, MT_RX_FILTR_CFG, dev->rxfilter);

	mutex_unlock(&dev->mutex);
}

static void
mt76_bss_info_changed(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		      struct ieee80211_bss_conf *info, u32 changed)
{
	struct mt76_dev *dev = hw->priv;
	struct mt76_vif *mvif = (struct mt76_vif *) vif->drv_priv;

	mutex_lock(&dev->mutex);

	if (changed & BSS_CHANGED_BSSID)
		mt76_mac_set_bssid(dev, mvif->idx, info->bssid);

	if (changed & BSS_CHANGED_BEACON_INT)
		mt76_rmw_field(dev, MT_BEACON_TIME_CFG,
			       MT_BEACON_TIME_CFG_INTVAL,
			       info->beacon_int << 4);

	if (changed & BSS_CHANGED_BEACON_ENABLED) {
		tasklet_disable(&dev->pre_tbtt_tasklet);
		mt76_mac_set_beacon_enable(dev, mvif->idx, info->enable_beacon);
		tasklet_enable(&dev->pre_tbtt_tasklet);
	}

	if (changed & BSS_CHANGED_ERP_SLOT) {
		int slottime = info->use_short_slot ? 9 : 20;

		mt76_rmw_field(dev, MT_BKOFF_SLOT_CFG,
			       MT_BKOFF_SLOT_CFG_SLOTTIME, slottime);
	}

	mutex_unlock(&dev->mutex);
}

static int
mt76_wcid_alloc(struct mt76_dev *dev)
{
	int i, idx = 0;

	for (i = 0; i < ARRAY_SIZE(dev->wcid_mask); i++) {
		idx = ffs(~dev->wcid_mask[i]);
		if (!idx)
			continue;

		idx--;
		dev->wcid_mask[i] |= BIT(idx);
		break;
	}

	idx = i * BITS_PER_LONG + idx;
	if (idx > 247)
		return -1;

	return idx;
}

static int
mt76_sta_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
	     struct ieee80211_sta *sta)
{
	struct mt76_dev *dev = hw->priv;
	struct mt76_sta *msta = (struct mt76_sta *) sta->drv_priv;
	struct mt76_vif *mvif = (struct mt76_vif *) vif->drv_priv;
	int ret = 0;
	int idx = 0;
	int i;

	mutex_lock(&dev->mutex);

	idx = mt76_wcid_alloc(dev);
	if (idx < 0) {
		ret = -ENOSPC;
		goto out;
	}

	msta->wcid.idx = idx;
	msta->wcid.hw_key_idx = -1;
	mt76_mac_wcid_setup(dev, idx, mvif->idx, sta->addr);
	mt76_clear(dev, MT_WCID_DROP(idx), MT_WCID_DROP_MASK(idx));
	for (i = 0; i < ARRAY_SIZE(sta->txq); i++)
		mt76_txq_init(dev, sta->txq[i]);

	rcu_assign_pointer(dev->wcid[idx], &msta->wcid);

out:
	mutex_unlock(&dev->mutex);

	return ret;
}

static int
mt76_sta_remove(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		struct ieee80211_sta *sta)
{
	struct mt76_dev *dev = hw->priv;
	struct mt76_sta *msta = (struct mt76_sta *) sta->drv_priv;
	int idx = msta->wcid.idx;
	int i;

	mutex_lock(&dev->mutex);
	rcu_assign_pointer(dev->wcid[idx], NULL);
	for (i = 0; i < ARRAY_SIZE(sta->txq); i++)
		mt76_txq_remove(dev, sta->txq[i]);
	mt76_set(dev, MT_WCID_DROP(idx), MT_WCID_DROP_MASK(idx));
	dev->wcid_mask[idx / BITS_PER_LONG] &= ~BIT(idx % BITS_PER_LONG);
	mt76_mac_wcid_setup(dev, idx, 0, NULL);
	mutex_unlock(&dev->mutex);

	return 0;
}

static void
mt76_sta_notify(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		enum sta_notify_cmd cmd, struct ieee80211_sta *sta)
{
}

static int
mt76_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
	     struct ieee80211_vif *vif, struct ieee80211_sta *sta,
	     struct ieee80211_key_conf *key)
{
	struct mt76_dev *dev = hw->priv;
	struct mt76_vif *mvif = (struct mt76_vif *) vif->drv_priv;
	struct mt76_sta *msta = sta ? (struct mt76_sta *) sta->drv_priv : NULL;
	struct mt76_wcid *wcid = msta ? &msta->wcid : &mvif->group_wcid;
	int idx = key->keyidx;
	int ret;

	if (cmd == SET_KEY) {
		key->hw_key_idx = wcid->idx;
		wcid->hw_key_idx = idx;
	} else {
		if (idx == wcid->hw_key_idx)
			wcid->hw_key_idx = -1;

		key = NULL;
	}

	if (!msta) {
		if (key || wcid->hw_key_idx == idx) {
			ret = mt76_mac_wcid_set_key(dev, wcid->idx, key);
			if (ret)
				return ret;
		}

		return mt76_mac_shared_key_setup(dev, mvif->idx, idx, key);
	}

	return mt76_mac_wcid_set_key(dev, msta->wcid.idx, key);
}

static int
mt76_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif, u16 queue,
	     const struct ieee80211_tx_queue_params *params)
{
	struct mt76_dev *dev = hw->priv;
	u8 cw_min = 5, cw_max = 10;
	u32 val;

	if (params->cw_min)
		cw_min = fls(params->cw_min);
	if (params->cw_max)
		cw_max = fls(params->cw_max);

	val = MT76_SET(MT_EDCA_CFG_TXOP, params->txop) |
	      MT76_SET(MT_EDCA_CFG_AIFSN, params->aifs) |
	      MT76_SET(MT_EDCA_CFG_CWMIN, cw_min) |
	      MT76_SET(MT_EDCA_CFG_CWMAX, cw_max);
	mt76_wr(dev, MT_EDCA_CFG_AC(queue), val);

	val = mt76_rr(dev, MT_WMM_TXOP(queue));
	val &= ~(MT_WMM_TXOP_MASK << MT_WMM_TXOP_SHIFT(queue));
	val |= params->txop << MT_WMM_TXOP_SHIFT(queue);
	mt76_wr(dev, MT_WMM_TXOP(queue), val);

	val = mt76_rr(dev, MT_WMM_AIFSN);
	val &= ~(MT_WMM_AIFSN_MASK << MT_WMM_AIFSN_SHIFT(queue));
	val |= params->aifs << MT_WMM_AIFSN_SHIFT(queue);
	mt76_wr(dev, MT_WMM_AIFSN, val);

	val = mt76_rr(dev, MT_WMM_CWMIN);
	val &= ~(MT_WMM_CWMIN_MASK << MT_WMM_CWMIN_SHIFT(queue));
	val |= cw_min << MT_WMM_CWMIN_SHIFT(queue);
	mt76_wr(dev, MT_WMM_CWMIN, val);

	val = mt76_rr(dev, MT_WMM_CWMAX);
	val &= ~(MT_WMM_CWMAX_MASK << MT_WMM_CWMAX_SHIFT(queue));
	val |= cw_max << MT_WMM_CWMAX_SHIFT(queue);
	mt76_wr(dev, MT_WMM_CWMAX, val);

	return 0;
}

static void
mt76_sw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif, const u8 *mac)
{
	struct mt76_dev *dev = hw->priv;

	tasklet_disable(&dev->pre_tbtt_tasklet);
	set_bit(MT76_SCANNING, &dev->state);
}

static void
mt76_sw_scan_complete(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct mt76_dev *dev = hw->priv;

	clear_bit(MT76_SCANNING, &dev->state);
	tasklet_enable(&dev->pre_tbtt_tasklet);
}

static void
mt76_flush(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
	   u32 queues, bool drop)
{
}

static int
mt76_get_txpower(struct ieee80211_hw *hw, struct ieee80211_vif *vif, int *dbm)
{
	struct mt76_dev *dev = hw->priv;

	*dbm = dev->txpower_cur;
	return 0;
}

static int
mt76_ampdu_action(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		  enum ieee80211_ampdu_mlme_action action,
		  struct ieee80211_sta *sta,u16 tid, u16 *ssn, u8 buf_size)
{
	struct mt76_dev *dev = hw->priv;
	struct mt76_sta *msta = (struct mt76_sta *) sta->drv_priv;

	switch (action) {
	case IEEE80211_AMPDU_RX_START:
		mt76_set(dev, MT_WCID_ADDR(msta->wcid.idx)+4, BIT(16 + tid));
		break;
	case IEEE80211_AMPDU_RX_STOP:
		mt76_clear(dev, MT_WCID_ADDR(msta->wcid.idx)+4, BIT(16 + tid));
		break;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
		ieee80211_send_bar(vif, sta->addr, tid, msta->agg_ssn[tid]);
		break;
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		break;
	case IEEE80211_AMPDU_TX_START:
		msta->agg_ssn[tid] = *ssn << 4;
		ieee80211_start_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		break;
	case IEEE80211_AMPDU_TX_STOP_CONT:
		ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		break;
	}

	return 0;
}

static void
mt76_sta_rate_tbl_update(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			 struct ieee80211_sta *sta)
{
	struct mt76_dev *dev = hw->priv;
	struct mt76_sta *msta = (struct mt76_sta *) sta->drv_priv;
	struct ieee80211_sta_rates *rates = rcu_dereference(sta->rates);
	struct ieee80211_tx_rate rate = {};

	if (!rates)
		return;

	rate.idx = rates->rate[0].idx;
	rate.flags = rates->rate[0].flags;
	mt76_mac_wcid_set_rate(dev, &msta->wcid, &rate);
}

const struct ieee80211_ops mt76_ops = {
	.tx = mt76_tx,
	.start = mt76_start,
	.stop = mt76_stop,
	.add_interface = mt76_add_interface,
	.remove_interface = mt76_remove_interface,
	.config = mt76_config,
	.configure_filter = mt76_configure_filter,
	.bss_info_changed = mt76_bss_info_changed,
	.sta_add = mt76_sta_add,
	.sta_remove = mt76_sta_remove,
	.sta_notify = mt76_sta_notify,
	.set_key = mt76_set_key,
	.conf_tx = mt76_conf_tx,
	.sw_scan_start = mt76_sw_scan,
	.sw_scan_complete = mt76_sw_scan_complete,
	.flush = mt76_flush,
	.ampdu_action = mt76_ampdu_action,
	.get_txpower = mt76_get_txpower,
	.wake_tx_queue = mt76_wake_tx_queue,
	.sta_rate_tbl_update = mt76_sta_rate_tbl_update,
};

void mt76_rx(struct mt76_dev *dev, struct sk_buff *skb)
{
	if (!test_bit(MT76_STATE_RUNNING, &dev->state)) {
		dev_kfree_skb(skb);
		return;
	}

	ieee80211_rx(dev->hw, skb);
}

