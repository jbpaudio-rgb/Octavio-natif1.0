/* SPDX-License-Identifier: MPL-2.0 */
/* LED layer: one level per LED (off / dim glow / breath / lit) written into the BSP's per-column arrays. The key LEDs
 * follow the keys (lit), the notes held by the arpeggiator glow, and its playing step breathes; BTN_ARP glows while
 * the selected track arpeggiates. The scene LEDs of the sequencer pages (step grids) will call leds_set() too. */
enum { LV_OFF, LV_DIM, LV_BREATH, LV_ON };

static void leds_set(uint32_t id, int lv)               /* id: button 0..13, note key 14..40 */
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id) {
                uint8_t b = (uint8_t)(1u << r);
                fm1_led[p] = (uint8_t)(lv == LV_ON ? fm1_led[p] | b : fm1_led[p] & ~b);
                fm1_led_dim[p] = (uint8_t)(lv == LV_DIM ? fm1_led_dim[p] | b : fm1_led_dim[p] & ~b);
                fm1_led_breath[p] = (uint8_t)(lv == LV_BREATH ? fm1_led_breath[p] | b : fm1_led_breath[p] & ~b);
            }
}

static void leds_update(void)
{
    static uint32_t sig;
    uint32_t keys = fm1_in.notes, btn = fm1_in.buttons, held, i, s;
    int t = voices_sel(), cur = voices_arp_note(t), base = ui_note_of_key(0);
    held = voices_held_keys(t, base);
    s = keys * 2654435761u ^ btn * 40503u ^ held * 69069u ^ (uint32_t)(cur + 1) * 977u ^ (uint32_t)voices_arp_get(t, ARP_MODE) * 31u ^ (uint32_t)base << 20;
    if (s == sig) return;                               /* only on a change */
    sig = s;
    for (i = 0; i < 14u; i++)
        leds_set(i, (btn >> i) & 1u ? LV_ON : (i == BTN_ARP && voices_arp_get(t, ARP_MODE)) ? LV_DIM : LV_OFF);
    for (i = 0; i < 27u; i++)
        leds_set(14u + i, (keys >> i) & 1u ? LV_ON : (cur - base == (int)i) ? LV_BREATH : (held >> i) & 1u ? LV_DIM : LV_OFF);
}
