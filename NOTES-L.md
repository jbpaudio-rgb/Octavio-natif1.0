# NOTES-L — octavio-native, branche m6-L (base d4de98d)

Quatre points : page ARP des drums, FX ORDER (réordonner), Prophet en émulateur, raccourcis/accords manquants. Rien n'est poussé ni flashé.
Références : le prototype a été rendu avec Playwright (`tools/ref_shots.js` + `tools/ref_shots.json`, horloge figée) pour les captures `proto_*` ; les listes de destinations ont été lues dans le DOM du prototype.

## 1. Page ARP des drums
Déjà faite par I (DSEQ, `EPI_ARP`, ARP tenu + OP1..6, section 0x42 DSQ rev 1). Ce que j'ai vérifié / ajouté :
* écran : `shots/drum_arp_page_native.png` vs `drum_arp_page_prototype.png` : erreur moyenne 0,58/255 (0,41 % de pixels > 24). Le graphe est vide comme le prototype (il appelle un `dgrey()` qui n'existe pas).
* effet sur le séquenceur : nouveau test hôte dans `tests/state_test.c` (0c) : chance 0 = aucun coup en 400 blocs, chance 100 % = joue ; swing 100 % sur le pas 1 retarde le coup de 22 blocs (0,5 pas). Humanisation / accent : même code (`seq_fire`), non mesurés séparément.
* sauvegarde : section 0x42 existante, aller-retour vérifié par `state_test` (inchangé, tout le tableau `dr[]` + gates est comparé).
* nouveau : geste « ALGORITHM puis SELECT dans les 5 s » sur une piste drums = kit des 6 voies (808 / 909 / ACOUSTIC), liste `ALGO · T4` avec rangée `DRUM · 808` et note `SELECT = KIT` (prototype : `algT`, `algList`, `dnE`). Avant, seul le kit d'une voie (voie tenue + ALGORITHM) existait. Captures `algo_*.png`. Sur la page HOME, SELECT reste le choix de piste (comme le prototype).
* noms d'engine : `MOOG` / `PROP` → `MINI` / `PRO70` (listes de presets, éditeur de nom, liste ALGO, noms par défaut), comme `dn()` du prototype.

## 2. FX ORDER : réordonner
Geste du prototype (curseur caché `fxpos`, `$("fxpos").addEventListener("input")`) : sur la page FX ORDER, K1 déplace la sélection d'une tuile (une molette = 1/50 de la course de 6 slots, donc ~10 crans par tuile) ; **SEL tenu + K1** déplace l'effet sélectionné (splice) et la sélection le suit ; relâcher SEL après un déplacement n'est pas un appui (sinon SEL seul = slot suivant). Les tuiles se redessinent (effet + tuile rouge qui suit).
* `fx_move()` (fxstate.c), `ctrl_fxo_knob()` (ctrl.c), K1 dans `ui_encoder` ; 4 slots (limite inchangée), sauvegarde 0x40 inchangée (l'ordre y est déjà). L'ordre est construit à part puis copié (l'audio ne voit jamais de doublon). Un changement d'ordre réalloue les lignes à retard de la piste (comme `fx_assign`) : les queues de delay/reverb de cette piste sont coupées.
* `shots/fxo_*.png` : OVERDRIVE FX1 → +20 crans avec SEL tenu → FX3 (Phaser, Flanger remontent), -10 → FX2, SEL relâché : l'ordre reste, la sélection aussi.
* FX tenu + touche blanche / OP5.. sur FX ORDER ou FX : plus de note (le prototype `play()` retourne). FX + OP8 : la sync du delay seulement si l'effet du slot est le delay (sinon on revient juste à la page FX), comme `toggleSync("FX")`.

## 3. Prophet en émulateur
Captures sur la piste 3 (HOME, SELECT ×2) : page LFO (liste des destinations parcourue au K1), cycle EDIT ×6 (FILTER, OSC A, OSC B, SUB, NOISE, FILTER), ENV ×2 (AMP ENV, FILTER ENV), GLO (MIXER), ARP, SEQ avec gates et lecture. Comparées au prototype (`proto_prophet_*.png`) :
* titres et cellules : 0,9 à 1,4 /255 d'erreur moyenne sur FILTER, OSC A/B/SUB/NOISE, AMP ENV, FILTER ENV, MIXER, LFO (les graphes d'oscillateur / bruit dépendent de l'horloge, 6 à 9 /255, non comparables pixel à pixel).
* liste des destinations du Prophet : comparée **par programme** à `select.md[data-k=m0t]` du prototype (Playwright, `rebuildDst`) : identique pour les 4 moteurs (VACT 69, MINI 78, PROPH 81, DRUM 80, hors « Seq gate length » ajoutée en natif ; listes de lignes de séquenceur : mêmes 3 premières entrées + liste moteur). Noms courts affichés (`Pr Blvl`, `Pr nCrsh`, `Phs De…`) tronqués comme dans le prototype.
Ce qui était faux et corrigé :
* **un select ne pouvait pas être changé par la molette** (ARP MODE, ENV MODE, formes d'onde, RANGE, TRANSP., etc.) : `ui_encoder`, `ep_encoder`, `epm_encoder` comparaient la position non arrondie à la position arrondie (±1) ; sur un select (2 à 25 valeurs) une molette de 1/50 ne dépassait jamais l'arrondi. Comme `vk()` du prototype (`|get() - wq| < 1e-9 ? ws : get()`), on mémorise maintenant la position arrondie laissée par le pas précédent. Constaté en émulateur (ARP, K1 +6 : MODE restait Off ; maintenant Up, `shots/arp_page_mode_up_K1.png`, et le Vactrol arpégé rejoue ses notes : `arp_up_vactrol.wav`).
* page SEQ : les valeurs de pas étaient **communes aux 4 pistes** (le Prophet affichait le triangle de la piste 1) ; le prototype a `TRX[t].SQ` par piste (piste 1 : triangle, les autres : zéros). Maintenant par piste (`sq_vt[4][6][16]`, +576 o), lues par piste dans `voices.c` / `lfo.c`. Section **0x44..0x47** (SQT, rev 1) ; l'ancienne 0x06 n'est plus écrite, mais encore lue (elle règle les 4 pistes, comme avant). `ST_MAX` 3200 → 3700.
* gates du séquenceur synthé : par défaut les 4 pistes jouaient (0x5555) ; le prototype n'active que la piste 1 (`GT` des autres = 0). Corrigé (dst.c). Un preset enregistré avec le build précédent et les gates par défaut omis (codage épars) rechargera zéro gate sur les pistes 2..4.
* page SEQ : les 3 gros potards ont la peau de l'engine (Minimoog : bague + point crème ; Prophet : sombre + trait blanc), sprites 54 px générés par Chrome depuis `kv()` (`assets/knob_big_{moog,pro}.json`, `tools/chrome_knobs.js`, `pack_assets.py`) : +23,5 Ko de flash. Le Vactrol et la batterie (pitch SEQ sur drums) gardent la peau bleue.
* page ARP : le prototype démarre à RATE 1/4 et TRANSP. -12 (le select commence sur sa première option) ; le natif voices avait 1/16 et 0. Défauts alignés (`ARP_DEF`). Surtout : **les pages ARP du synthé n'étaient pas reliées à l'arpégiateur** (`PV[P_ARP*]` édité, `voices_arp` jamais appelé) : K1..K4 appellent maintenant `voices_arp(piste sélectionnée)`, la page affiche l'arpégiateur de la piste (décalages octaves +1, transposition -12, répétition +1).
* REC pendant PLAY sur la page SEQ : K3 (STEP VALUE) écrit le pas en cours (`rec&&auto?cur:ss`).
* LED des touches sur la page SEQ : allumée si |valeur| ≥ 4 % (positive = allumée, négative = tamisée), pas en cours = respiration (le prototype module l'intensité ; la LED du FM-1 n'a que on / tamisé / respiration).

## 4. Accords et raccourcis du prototype
Comparaison avec `b.onpointerdown` des touches, `btn()` et les gestionnaires des boutons du prototype :
| geste | avant | maintenant |
|---|---|---|
| HOME tenu + OP1..4 (page HOME) : piste | absent | fait |
| ARP tenu + OP1..3 (page ARP, synthé) : ARP / ARP 2 / ARP 3 | absent (ARP2 / ARP3 inatteignables) | fait |
| LFO tenu + OP1..6 (page LFO) : source | absent | fait |
| LFO tenu + OP8 : sync tempo du LFO | absent | fait |
| SEQ tenu + OP1..6 (page SEQ) : ligne | absent | fait |
| page SEQ, touche blanche : sélectionne le pas ; appui court sur un pas neutre = 1,0 ; second appui < 350 ms sur la même ligne = 0 ; maintien ≥ 350 ms = sélection seule | absent | fait (prototype : `pointerup` de la touche) |
| FX tenu + OP8 : sync du delay | fait | seulement si l'effet est le delay |
| ALGORITHM puis SELECT (drums) : kit | absent | fait |
| REC + K3 sur SEQ : écrit le pas en cours | absent | fait |
| EDIT/GLO/ENV + OPk (OSC, ENV2, voix drums), FX + OP1..4 / touche blanche, drum ARP + OP, DSEQ (voie, pas), HOME + touche blanche (banques), SAVE… | fait | inchangé |
Ajout natif (absent du prototype, nécessaire sans panneau web) : SEQ tenu + touche blanche = bascule du gate du pas (le prototype ne les édite que sur son panneau web).
Non repris, volontairement : OP8 seul ouvre FX ORDER depuis n'importe quelle page dans le prototype ; ici seulement depuis les pages FX (demande précédente, NOTES-I). FX ORDER a 4 tuiles (prototype 6) : FX tenu + OP5/OP6 n'existent pas. Les raccourcis clavier d'ordinateur sont ignorés. Pas de toast `say()` (PLAY / REC / « Not assigned » / Octave / Track n) : dans le prototype ils vont dans la ligne `#l1/#l2` qui n'apparaît pas dans l'écran de 240 px.

## Mémoire
`_bss_end` 0x01C706D8 → **0x01C70B28** (+1 200 o : step values par piste 576, tampon `ebuf` +500, états de touches / molettes) ; reste 0x34D8 = 13,5 Ko sous 0x01C74000 (avant : ~14,8 Ko). Flash image 540 440 → 564 084 o (fenêtre XIP 581 564 : 17 Ko restants).
Capacité des presets (état artificiel à 25 % de valeurs éditées) : 20 → 17 ; pire cas 10 → 8 (la section SQT fait 96 valeurs × 4 pistes). Un preset typique reste de l'ordre de quelques centaines d'octets.

## Tests
* `gcc -O1 -w -Isrc -Ibsp/chip -o /tmp/state_test tests/state_test.c && /tmp/state_test` : ALL OK (0 échecs), aussi sous `-fsanitize=address,undefined`. Ajouts : 0a (valeurs de pas par piste, 0x06 hérité règle 4 pistes, non écrit), 0c (chance / swing des lanes).
* émulateur (`fm1-emu`, image `build/octavio.fwsc`) : captures dans `shots/` ; chaque geste du tableau ci-dessus a été joué sauf ce qui est listé ci-dessous.

## Non testé / limites
* Rien sur matériel. Le temps réel du réordonnancement (réallocation de 72 Ko de pool à chaque cran de SEL + K1) n'est pas mesuré ; le premier bloc suivant est plus long, comme après `fx_assign`.
* Non rejoué en émulateur : ARP + OP1 (retour à ARP), HOME + OP4, LFO + OP1 / OP2 / OP4..6, SEQ + OP autres que OP2, REC + K3, LED des touches sur la page SEQ (aucune lecture de LED dans l'émulateur), peau Minimoog des gros potards (une capture, pas de comparaison pixel).
* Humanisation et accent de la page ARP drums : non mesurés (même chemin de code que chance / swing, relu contre `fire()`).
* Les presets enregistrés avant ce commit : gates par défaut et valeurs de pas globales (0x06) : voir plus haut.
* `fxbank_test` (tools/fxhost.sh) non relancé.
Aide : tools/emu_run.sh NOM FWSC MS SNAPS KEYS [w] (captures PNG dans /tmp/L/run/NOM/work).
