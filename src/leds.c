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

static const int8_t LED_CB[14] = { -1, -1, CB_FX, CB_SEL, CB_ENV, CB_LFO, CB_EDIT, CB_GLO, CB_HOME, CB_SAVE, CB_ARP, CB_SEQ, CB_PLAY, CB_REC };
static void leds_update(void)
{
    static uint32_t sig;
    uint32_t keys = fm1_in.notes, btn = fm1_in.buttons, held, i, s;
    int t = voices_sel(), cur = voices_arp_note(t), base = ui_note_of_key(0);
    held = voices_held_keys(t, base);
    s = keys * 2654435761u ^ btn * 40503u ^ held * 69069u ^ (uint32_t)(cur + 1) * 977u ^ (uint32_t)voices_arp_get(t, ARP_MODE) * 31u ^ (uint32_t)base << 20 ^ ctrl_lit() * 7919u ^ ctrl_locked() * 104729u ^ (uint32_t)(ctrl_bank_mode() ? ctrl_bank_cur() + 2 : 0) * 15485863u;
    if (s == sig) return;                               /* only on a change */
    sig = s;
    for (i = 0; i < 14u; i++)
        leds_set(i, (btn >> i) & 1u || (LED_CB[i] >= 0 && (ctrl_lit() >> LED_CB[i]) & 1u) ? LV_ON :
                    (i == BTN_ARP && voices_arp_get(t, ARP_MODE)) ? LV_DIM : LV_OFF);
    if (ctrl_bank_mode()) {                             /* HOME held / SAVE BANK: only the current bank's white key (leds(): .led[data-w] = curBank) */
        static const uint8_t WH[12] = { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1 };
        int w = 0;
        for (i = 0; i < 27u; i++) {
            int white = WH[(uint32_t)(base + (int)i) % 12u];
            leds_set(14u + i, white && w == ctrl_bank_cur() ? LV_ON : LV_OFF);
            w += white;
        }
        return;
    }
    for (i = 0; i < 27u; i++)
        leds_set(14u + i, (keys >> i) & 1u ? LV_ON : (cur - base == (int)i) ? LV_BREATH : (held >> i) & 1u ? LV_DIM : LV_OFF);
}
