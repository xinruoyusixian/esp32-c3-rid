# -*- coding: utf-8 -*-
"""把 collect_uav 换成干净版本（不改动 tracker 表）"""
import io, os, re

P = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "main", "crid_screen.c")
src = io.open(P, encoding="utf-8").read()

start = src.index("static void collect_uav(uav_stat_t *st)")
end = src.index("/* ---------------- 各模式页面 ---------------- */")
old = src[start:end]

new = '''static void collect_uav(uav_stat_t *st)
{
    memset(st, 0, sizeof(*st));

    SemaphoreHandle_t m = crid_tracker_get_mutex();
    if (!m) return;
    if (xSemaphoreTake(m, pdMS_TO_TICKS(50)) != pdTRUE) return;

    uav_track_t *tab = crid_tracker_get_table();
    int cap = crid_tracker_get_capacity();
    if (!tab || cap <= 0) { xSemaphoreGive(m); return; }
    if (cap > 32) cap = 32;          /* 位图只有 32 位，够用了 */

    uint32_t used = 0;               /* 已挑走的槽位 */

    /* 两轮：先挑活跃的，没挑满再补"最近见过但已超时"的 */
    for (int pass = 0; pass < 2 && st->n < TGT_MAX; pass++) {
        while (st->n < TGT_MAX) {
            int bi = -1;
            uint32_t bseen = 0;
            for (int i = 0; i < cap; i++) {
                if (used & (1u << i)) continue;
                uav_track_t *u = &tab[i];
                /* 跳过空白槽位 */
                if (!(u->mac[0] | u->mac[1] | u->mac[2] |
                      u->mac[3] | u->mac[4] | u->mac[5])) continue;
                if (pass == 0) {
                    if (!u->active) continue;
                } else {
                    if (u->active) continue;   /* 活跃的上一轮已挑完 */
                }
                if (u->last_seen_ms <= bseen) continue;
                bseen = u->last_seen_ms;
                bi = i;
            }
            if (bi < 0) break;
            used |= (1u << bi);

            uav_track_t *u = &tab[bi];
            tgt_t *o = &st->t[st->n++];
            const char *uas = u->basic_id.valid ? u->basic_id.uas_id : "";
            if (uas[0]) snprintf(o->id, sizeof(o->id), "%.12s", uas);
            else        snprintf(o->id, sizeof(o->id), "%02X%02X%02X",
                                 u->mac[3], u->mac[4], u->mac[5]);
            o->spd  = u->location.speed_horizontal;
            o->alt  = u->location.height;
            o->hdg  = u->location.direction;
            o->rssi = (int)u->last_rssi;
        }
    }

    /* 统计总数（只读，不改表） */
    for (int i = 0; i < cap; i++) {
        uav_track_t *u = &tab[i];
        if (!(u->mac[0] | u->mac[1] | u->mac[2] |
              u->mac[3] | u->mac[4] | u->mac[5])) continue;
        st->total++;
        if (u->active) st->online++;
    }
    xSemaphoreGive(m);
}

'''

src = src[:start] + new + src[end:]
io.open(P, "w", encoding="utf-8").write(src)
print("collect_uav replaced, %d -> %d bytes" % (len(old), len(new)))
