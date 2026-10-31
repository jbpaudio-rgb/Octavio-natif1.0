# NOTES-M — drums sur 1 piste, Prophet polyphonique, volumes HOME, SELECT sur DSEQ (mise à jour 2)

> **Mise à jour 2 (cette section prime sur le reste du document).** Décisions de l'utilisateur : **UNE seule piste de drums (piste 4), 6 voix** ; le passage à 4 voix est en attente (rien de fait dans `m7-M`, la variante B n'est que mesurée en §7). Les §1-8 ci-dessous décrivent l'état précédent (drums sur les pistes 3 et 4) : les chiffres « 2 pistes drums » ne valent plus pour le firmware actuel.

## 0. Quatre corrections

### 0.1 Fader de volume HOME sans effet (corrigé)
**Cause** : `PV[P_VOL0..3]` n'était lu que par l'écran HOME ; le mixage utilisait `TK[t].vol`, que seul le chargement d'une scène écrivait (`voices_track_level`). Tourner le fader ne changeait donc rien au son. **Correctif** (`voices_render`) : le volume de piste est lu dans `PV[P_VOL0 + t]` à chaque bloc et appliqué en rampe sur le bloc, après le FX de la piste, donc l'ordre est moteur → gain Level de modulation → chaîne FX → volume → somme (comme le prototype `F(tb[i]) * t.vol`), pour les 4 moteurs.
**Test** (`tests/state_test.c`, 7c) : rms proportionnel au volume (240 / 120 / 60 / 0 sur le Minimoog, le Prophet et les drums, tolérance +/-30 % : le bus a un `tanh`, et les moteurs ne sont pas rigoureusement stationnaires) ; **le test échoue (10 échecs) avec l'ancien code et passe avec le correctif**. Scène : volumes 300 / 650 enregistrés puis relus (`scene_save` / `scene_load`) ; les volumes sont dans la section MIX (0x07), **scènes seulement, comme le `q.vol` du prototype** : un preset ne les enregistre pas (non modifié, c'est la sémantique du prototype).
**Émulateur** : `audio/volume-*.wav` (pvol_m Minimoog, pvol_p Prophet 2 notes, pvol_d drums) : le fader (HOME + K) descend, le rms par fenêtre de 0,3 s tombe à 0 en quatre crans (Minimoog 1644 -> 2383 -> 1234 -> 894 -> 382 -> 0 ; drums 4562 -> 2959 -> 2402 -> 924 -> 0).

### 0.2 Prophet limité à 2-3 voix (corrigé)
**Cause** : le gouverneur CPU. Avec Vactrol + Minimoog + drums actifs, effets par défaut et modulation (charge ~50 %), il retirait des voix aussi au Prophet (la classe « la plus chère » perdait des voix, plancher 2 voix en tout). Reproduit dans fm1-emu avec le vrai chemin des touches (5 touches tenues sur le Prophet piste 2, 3 autres pistes en jeu, gouverneur actif, `pk5`) : **avant (d716ede) 3 fondamentales sur 5** (F3 et G3 à 3 dB au-dessus de la médiane = absentes) ; **après 5 sur 5** (14-17 dB au-dessus de la médiane). Sans charge (firmware réel, 5 touches en même temps) le Prophet donnait déjà 5 voix : ce n'est donc ni le mode MONO/POLY, ni une limite par piste, ni le vol de voix.
**Correctif** : ordre de délestage = effets, puis voix de drums jusqu'à un plancher de 2, et **le Prophet seulement sous charge sévère** (bloc > 80 % ou fenêtre > 70 %), jamais sous 3 voix. Sous charge normale le gouverneur ne touche plus aux accords.
**Limite honnête** : l'attaque d'un accord de 5 notes coûte à elle seule ~+25 % de bloc d'un coup ; `pk5` montre un bloc à 75,7 % (11 blocs entre 65 et 70 %) à l'instant où l'accord est joué, le gouverneur retombe à ~51 % en quelques blocs en retirant des effets. Pas de perte de bloc (0 trou, 0 bloc rejoué).

### 0.3 SELECT = voix de drum sur DSEQ + LED du clavier
Sur la page DSEQ (séquenceur de drums), **SELECT seul** passe à la voix suivante / précédente (1 cran = 1 voix, bouclage 6 -> 1 et 1 -> 6) ; un toast « LANE n  kit voix » s'affiche, la lane sélectionnée est cerclée de rouge à l'écran (déjà le cas), et **la touche noire OPk de la voix s'allume** (`markSel()` du prototype : OPk = k-ième touche noire) ; idem sur les pages EDIT / ENV / ARP d'une piste drums (touche en lueur). Gestes inchangés : OPk tenu / double appui = lane tenue / verrouillée (un verrou suit SELECT), ARP + OPk, SEQ tenu + SELECT = type de la voix, ALGORITHM puis SELECT (5 s) = kit. Le prototype n'a pas SELECT = voix (c'est un ajout demandé).
**Vérifié** : test hôte (`state_test` 7d) des 6 voix avec bouclage dans les deux sens, de l'état des LED (une seule touche noire, la bonne, pour chaque voix ; lueur en EDIT), SEQ + SELECT inchangé, verrou ; captures fm1-emu avant/après (`shots/dseq-select-before.png` : la lane reste BD ; `shots/dseq-select-after.png` : BD -> SD -> CH -> CP -> CB avec bouclage). Les LED réelles de l'émulateur ne sont pas exposées : seul le test hôte de `leds_update` les vérifie.

### 0.4 Une seule piste de drums (piste 4)
- Choix : **piste 4**, comme le prototype (piste 4 = drums par défaut) et l'état d'usine actuel ; les pistes 1-3 n'ont plus DRUM dans la liste ALGORITHM (note « TRACK n (DRUMS T4) ») ; un preset / une scène avec DRUM ailleurs charge un Minimoog.
- Format : section **DSQ rev 3** (une piste, 6 lanes de 16 bits + 4 réglages x 6) ; DSQ rev 2 (2 pistes) et rev 1 (4 pistes) relues (seule la piste 4 est gardée, testé) ; sections moteur : 9 au lieu de 10 (plus de drums piste 3), renumérotation de la liste ; le test d'ancien enregistrement passe.
- Gouverneur : 6 voix de drums au plus, plancher 2 ; budget FX/voix inchangé (seuils 56 / 51 / 47 / 80 %).
- **RAM** : `DRM` 1 192 o (1 piste) contre 2 384 o (2 pistes) et 4 768 o (4 pistes, m6) : **−1 192 o récupérés vs le build précédent, −3 576 o vs m6**. RAM libre sous 0x01C74000 : 11 684 o (précédent 10 484, m6 12 916). Image flash : 568 104 o (précédent 567 912 : +192 o de code pour SELECT / LED / DSQ rev 3).

### 0.5 Mesures avant / après (fm1-emu, firmware LOADTEST, 3,8-6 s ; bloc = 69 632 ticks)

| Cas | Précédent (d716ede, 2 pistes drums) | Maintenant (1 piste drums) |
|---|---|---|
| Piste drums, 6 voix, coût brut | 21 868 (31 %) | 18 215-18 286 (26 %) |
| **Pire cas** : Vactrol + Minimoog + Prophet 5 notes + drums + FX lourds + 24 routes, **brut** | 119,9 % moy., 125 % pire (2 pistes drums) | **89,7 % moy., 93,1 % pire** |
| Même cas, **gouverné** | moy. 46,0 %, pire 62,0 % | **moy. 45,1 %, pire 51,8 %**, 7 slots FX retenus, 0 blocs > 75 % |
| Cas utilisateur Vactrol + Minimoog joué + drums + FX + modulation (um_fx) | moy. 51,4 %, pire 63,8 % | moy. 50,9 %, pire 68,9 % (2 blocs à 65-70 % pendant l'installation, ensuite <= 56 %) |
| 4 pistes : Vactrol + 2 Moog + drums (wm) | moy. 47,6 %, pire 55,9 % | moy. 47,1 %, pire 49,5 % |
| 3 Prophet 5 notes + drums, effets, modulation (wc) | moy. 40,6 %, pire 67,4 % | moy. 43,9 %, pire 56,5 % (17 frontières perdues au démarrage seulement) |
| Accord 5 touches sous charge (pk5) | 3 voix audibles | 5 voix, pire bloc 75,7 % à l'attaque de l'accord |

Cible « pire bloc bien sous 55 % » : atteinte pour le pire cas gouverné (51,8 %), wm (49,5 %) et wc (56,5 %, à peine au-dessus) ; **pas atteinte** pour um_fx (68,9 % sur 2 blocs au démarrage) et pk5 (75,7 % à l'attaque d'un accord de 5 notes) : ce sont des pics ponctuels, pas de la charge soutenue (le palier est à 50-51 %).

### 0.6 Non testé
Puce réelle ; écoute ; LED physiques ; capture d'une page drums sur matériel ; le variant 4 voix (en attente, rien codé). Le test de volume est un test de rms avec tolérance (+/-30 %), pas une courbe exacte.

---

Branche `m7-M` (base `e8422b5`). Rien n'est poussé ni flashé. Tout est mesuré dans **fm1-emu** (le CPU de l'émulateur n'est pas la puce ; aucune écoute humaine). Unité : 1 bloc de 128 trames = 69 632 ticks = 100 %.

## 1. Ce qui change

| Sujet | Décision |
|---|---|
| Drums | seulement **pistes 3 et 4** (t = 2, 3), comme le prototype (piste 4 = drums). La liste ALGORITHM des pistes 1-2 n'a plus DRUM (3 moteurs, note « DRUMS T3 T4 »). `voices_set_engine` refuse DRUM sur t<2. Un preset / une scène avec drums sur 1-2 est réparé au chargement : la piste devient Minimoog. |
| Format | section DSQ rev 2 (2 pistes de séquenceur drums) ; DSQ rev 1 relu (pistes 1-2 ignorées) ; sections moteur sans t0/t1 drums. `tests/state_test.c` vert (ALL OK, 0 failure), `fxbank_test` vert. |
| Prophet | **5 voix** par piste, vol de voix sans reset (l'attaque repart du niveau courant), release par note, enveloppes par voix, bus : DC-block une fois par piste + `c_tanh` (pas d'écrêtage). Arp / séquenceur : un note-off par note. Le Minimoog reste mono. |
| Drums | voix coupées par un fondu de 32 échantillons (choke du charley, gouverneur) au lieu d'une coupure ; enveloppes à pas de contrôle (8 éch.) ; `exc()` mis en cache. |
| Gouverneur | voir §3. |

## 2. Coûts (ticks par bloc, firmware LOADTEST, gouverneur coupé = coût brut)

| Mesure | Avant (m6) | Après (M) |
|---|---|---|
| Piste Prophet, 1 note | 4 910 (7,1 %) mono | 3 868 (5,6 %) |
| Prophet, 3 notes | n/a | 10 617 (15 %) |
| Prophet, 5 notes (plein) | n/a | 17 370 (25 %) → **≈ 3,4 k ticks (4,9 %) par voix** |
| Minimoog (mono) | 7 535 (10,8 %) | 7 534 |
| Piste drums, 6 voix | 30 630 (44 %) | **21 868 (31,4 %)** |
| Une voix de drums seule (voix 0..5) | – | 4 932 / 4 948 / 4 242 / 4 230 / 4 334 / 4 571 |

## 3. Glitchs « plus de son » à 3 pistes (Vactrol + Minimoog + drums, LFO/matrice, PLAY, effets par défaut)

**Cause trouvée** (reproduite en marge, pas en trou audio) : la machine tournait à **71 % de bloc en moyenne, 79 % au pire** dans ce cas (um_fx), et le gouverneur de m6 agissait par basculements durs :
- seuil trop haut (≈ 80 %) : aucune marge pour les pics (alloc d'un slot FX, pas de séquenceur, noteOn) ;
- tout slot FX / voix coupé ou remis **en un bloc, sans fondu** → clics ;
- drums : choke du charley en coupure sèche ; vol de voix du gouverneur sans fondu.

Non trouvé : débordement/saturation, ni pic de ticks au noteOn du Minimoog (mesuré : clé → bloc 2,4 ms moyenne, pas de pic). Non reproduit : de vraies pertes de bloc (underruns) dans fm1-emu avec le firmware m6 (uf 0-1 au démarrage). Le correctif est donc **réduire le coût + des marges + des fondus**, pas la correction d'un bug unique : à vérifier sur la vraie puce.

**Correctifs** :
- Coût : drums 44 % → 31 % de bloc par piste (exc en cache, enveloppes à pas de contrôle).
- Gouverneur : seuil = moyenne des 8 derniers blocs ; **retire à 56 %, vise 51 %, réadmet à 47 %**, urgence 80 % sur un seul bloc ; délais de refroidissement séparés FX/voix avec back-off ; les ticks d'allocation FX exclus de la mesure ; plafonds de voix drums ≤ 12 / Prophet ≤ 20 ; la classe la plus chère s'allège en premier, FX d'abord.
- **Fondus** sur toutes ses actions : slot FX = crossfade sec↔mouillé sur un bloc ; voix drums = fondu 32 éch. ; notes tenues du Prophet = suspendues puis reprises (fondu sur un bloc), pas tuées ; le choke charley = fondu.
- Un pic de ≈ +30 % à chaque pas de séquenceur apparaissait dans le nouveau code (cause : le `kill` de drums durait un bloc entier et re-déclenchait le calcul de tout le bloc) : fondu par échantillon, pic supprimé (pire bloc de wm 77 % → 56 %).

| Cas (fm1-emu, 6 s) | moy. | **pire bloc** | blocs > 75 % | WAV : trous / blocs rejoués |
|---|---|---|---|---|
| m6 : Vactrol + Minimoog joué + drums + FX (um_fx) | 71,4 % | **79,3 %** | 2 | 0 / 0 |
| M : même cas | 51,4 % | **63,8 %** | 0 | 0 / 0 |
| m6 : u_vmd_fx_mod (modulation, séquenceurs) | 70,7 % | 71,4 % | 0 | 0 / 0 |
| M : Vactrol + Minimoog + 2 drums + accord 5 notes + FX + 24 routes (wm) | 47,6 % | **55,9 %** | 0 | 0 / 0 |

Cible « pire bloc < 50-55 % » : **non atteinte strictement** (63,8 % en um_fx, 56 % en wm) ; elle l'est en moyenne (≈ 51 % = valeur visée). Le gouverneur retient ensuite ~7 slots FX et plafonne les voix.

Honnêteté : le compteur de « clics » du banc passe de 29 à 165 dans um_fx, mais l'amplitude du plus fort (dérivée seconde) tombe de 2 589 à 886 et les événements se concentrent dans les 1,5 premières secondes (démarrage / gouverneur qui s'installe, signal faible → seuil relatif bas) ; je ne l'interprète pas comme des clics audibles, sans pouvoir l'affirmer (pas d'écoute). Les « frontières perdues » (16-17 dans wc / wf) arrivent toutes dans les 800 premières ms (démarrage, avant que le gouverneur n'installe ses plafonds) ; zéro ensuite (vérifié par runs de 400 / 800 ms / 3,8 s : 6 / 17 / 17).

## 4. Budget du pire cas (gouverneur actif)

Pire cas = 2 pistes drums × 6 voix + Minimoog + Prophet 5 notes + FX lourds + 24 routes de modulation (wf) : **brut 120 % de bloc (impossible)**, gouverné : moy. 46,0 %, pire 62,0 %, 7 slots FX retenus, plafond drums descendu à 2-3 voix. Autres pires cas gouvernés : wc (2 Prophet × 5 notes + 2 drums) moy. 40,6 % pire 67,4 % ; wm 47,6 % / 55,9 % ; 4 Prophet × 5 notes ≈ 42 % (m6 f4 non rejoué après le dernier changement). Les drums ne sont pas volés avant le Prophet sans raison : la classe la plus chère s'allège d'abord (le drum la plupart du temps), les notes du Prophet sont suspendues (pas perdues).

## 5. Mémoire (fm1-emu, `bss_end`)

| | Avant | Après |
|---|---|---|
| RAM libre sous 0x01C74000 | 12 916 o | **10 484 o** (−2 432 o : état poly Prophet 3 440 o (`PR`), arp/séquenceur per-note, tampons du gouverneur ; **net après** le gain des drums) |
| `DRM` (drums) | 4 × 1 192 = 4 768 o | 2 × 1 192 = **2 384 o** (**−2 384 o récupérés** grâce à la limite à 2 pistes) |
| Image flash | 564 084 o | 567 912 o (+3,8 Ko) ; fenêtre XIP 581 564 : ≈ 13,6 Ko restants |
| Piles | – | pile user jamais touchée sur 17 Ko / 24 Ko (7,2 Ko utilisés) ; pile IRQ 3 Ko / 7,9 Ko |

On ne dépasse pas les 13 Ko RAM / 14 Ko XIP annoncés ; il reste 10,5 Ko RAM.

## 6. Vérifications dans fm1-emu (wav + écran)

- **Accord 5 notes** (`pchord`, Prophet piste 2, notes tenues) : la FFT (`tools/chordfft.py`) montre les 5 fondamentales F3 175 / G3 196 / A3 221 / C4 262 / D4 295 Hz, de 0 à −4 dB du pic (fichier `audio/poly-prophet-chord-release-steal.wav`).
- **Release par voix** : lâcher des touches à 2,1 / 2,4 / 2,7 / 3,0 / 3,3 s (temps du wav) : chaque fondamentale décroît à son propre instant (tableau dans `audio/release.txt` : F3 126 → 83 dB entre 2,1 et 2,8 s, D4 reste à ~131 jusqu'à 3,3 s).
- **Vol sans clic** : 6e touche (E4) avec 5 tenues : la plus ancienne (F3) disparaît (−37 dB), E4 apparaît à 0 dB ; plus grand saut d'échantillon 1 718 contre un 99,9e centile de 1 405 (pas de discontinuité hors de la forme d'onde).
- **Drums 2 pistes + Prophet poly + effets** (wc, wm, wf) : wav sans trou ni bloc rejoué (`holes 0`, `repeated 0`), rms 5 000 - 5 900, 0 underrun hors démarrage.
- Écran : la liste ALGORITHM de la piste 1 montre VACT / MINI / PRO70 et « TRACK 1 (DRUMS T3 T4) » (`shots/`). **Pas de capture d'une piste drums** : je n'ai pas réussi à sélectionner la piste 3/4 par le script de touches du banc (HOME + enc0) ; non vérifié visuellement.

## 7. Comparaison des variantes de moteur drums (mesures seulement ; la branche garde A)

| | **A : 6 voix (branche)** | **B : 4 voix** | **C : 6 voix allégées** |
|---|---|---|---|
| Ticks, 1 piste drums | 21 868 (31,4 %) | 15 119 (21,7 %) : −31 % | 20 508 (29,5 %) : −6 % |
| Pire cas total (wf) gouverné, moy. / pire | 46,0 % / 62,0 % | 45,7 % / 57,1 % | 45,1 % / 60,9 % |
| Pire cas wf **brut** (sans gouverneur) | 119,9 % | 102,9 % | 116,4 % |
| Écart au A (rms / bandes) | – | rms −8 % ; −3,5 dB à 100-250 Hz, −2,5 dB à 250-500 Hz (les 2 voix retirées sont les toms / cloche) | rms identique ; ≤ 0,2 dB par bande (0-22 kHz) ; erreur d'onde 3 % |
| RAM économisée | – | 2 voix × 2 pistes dans `DRM` : ≈ 0,8 Ko (état voix 6 → 4) | 0 |
| Flash | – | ≈ 1 Ko (types 4/5 de voix inutilisés si lanes retirées) | ≈ 0 |
| À changer | – | mixeur à 6 faders → 4 ; DSEQ 6 lignes → 4 ; presets : sections DRM / DSQ rev 3 ; pages UI (6 lignes → 4) ; DRUM de l'ALGORITHM / kits ; séquenceurs rev 3 | rien (`-DDRUM_LITE`, patch de 43 lignes dans `variant-C-drum-lite.patch`, branche `m7-M-drumC`) |

- B : les lanes 0-3 sont kick, snare, charley fermé, charley ouvert ; ce sont celles du motif par défaut du prototype (kick, snare, hats), la clap/tom et la cloche/ride sautent. Mesuré avec `-DLT_DLANES=15` (les 2 lanes retirées simplement muettes : le coût est celui de 4 voix, la RAM est une estimation) — **ce n'est pas une implémentation** à 4 voix.
- C : le gain est faible parce que le coût est dans la synthèse même (oscillateurs, filtres, bruit), pas dans l'enrobage. L'allègement qui coûterait vraiment moins (rendu à 22 kHz) abîmerait les charleys (> 11 kHz) et touche fréquences / filtres / pas de temps de toutes les voix : non fait. Pas de comparaison au prototype lui-même (pas de WAV de référence du prototype ici) : l'écart est donné contre A.
- Dans les 3 cas le pire cas total reste très au-dessus de 100 % brut : c'est le gouverneur (voix plafonnées, FX retenus) qui rend la charge tenable, pas le choix A/B/C.

## 8. Non testé / limites

- fm1-emu n'est pas la puce : ticks et pires blocs sont des mesures de l'émulateur ; le glitch de l'utilisateur n'a **pas été reproduit comme perte de blocs** ; le correctif réduit coût et marge et ajoute les fondus, son effet réel est à confirmer sur le matériel / émulateur web.
- Aucune écoute humaine.
- La combinaison wc (2 Prophet à 5 notes + 2 drums) tourne à 40 % mais le gouverneur plafonne les voix à 2-3 pendant ~1 s au démarrage avant de se stabiliser.
- Pas de capture d'écran d'une piste drums.
- Le banc `tools/loadtest.py run <variante>` reproduit tout (variantes `wc wm wf pchord drB um_fx` ; env `JIELI_TOOLCHAIN AC79_SDK LT_OUT`).

## Mise à jour 3 : charge, glitch à 3 pistes, indicateur debug, DSEQ

**Honnêteté d'abord** : l'objectif "pire bloc <= 35 % dans fm1-emu" pour Vactrol + Drum + Prophet 5 notes n'est **pas atteint**. Le plancher brut des moteurs (sans gouverneur) est d'environ 47-69 % selon le motif ; avec le gouverneur : u3 (3 pistes, motif drums chargé) moyenne 37,2 % / pire bloc 49,4 % ; pk5 (5 notes Prophet) 43,6 / 62,1 % (avant : 78,7 %). Rien n'a été testé dans l'émulateur web de l'utilisateur, ni sur silicium, ni à l'oreille.

### Indicateur de charge DEBUG
Geste : **HOME maintenu + REC** (bascule). Panneau en haut de l'écran, 6 lignes : CPU moyen / pire bloc sur 1 s / pire depuis le début ; LOST (frontières perdues), ISR JITTER ; VOICES (Prophet, drums, Minimoog) ; FX (en marche / bypassés par le gouverneur) ; CAPS (plafonds drums / Prophet, fps UI) ; "ECO" affiché quand le Prophet est en demi-cadence. Test hôte `state_test` 7e. Capture : `shots/hud-debug.png`. Demander à l'utilisateur de lire ces chiffres dans SON émulateur.
Note : "ALL" (pire depuis le début) inclut le pic du premier bloc, à ignorer.

### Profil par fonction (u3r : 3 pistes, tous les motifs, sans gouverneur, instructions par bloc de 128 trames, `tools/profile.py`)
drum_render 119,6k (23,8 %) ; voix Prophet (inlinée, 2 symboles) 132k (26 %) ; fx0-fx3 125k (24,9 %) ; exc 26k ; met 20k ; vac_render 19,7k ; timer5 (scan touches) 18,7k ; exp2_fx 10,9k ; le reste < 1 %. Avant : drums ~35 % de l'ISR (drum_render 125k + exc 27k + met 21k), Prophet 23 %, FX 26 %.
Optimisations : suiveur drums tous les 4 échantillons, flanger/chorus à cadence contrôle (cible de retard tous les 8 échantillons, rampe linéaire), `c_tanh` inline, gain voix Prophet par bloc. Gain brut modeste : environ -7 % (FX -19 %). Le vrai gain vient de **ECO** : voix Prophet calculées à demi-cadence + interpolation (-39 % sur le Prophet, rien au-dessus de 11 kHz, images ~ -70 dB entre 15 et 22 kHz), qui conserve la polyphonie.

### Gouverneur : ordre et planchers
Seuils : shed 50 %, aim 44 %, admit 40 %, urgence 75 % (moyenne des 8 derniers blocs). Ordre de réduction : 1) effets, 2) ECO (Prophet demi-cadence, déclenché de façon prédictive au début d'un accord), 3) voix drums (plancher 2), 4) voix Prophet **seulement si charge sévère** (moyenne > 70 % ou un bloc > 75 %), plancher **3**, avec fondu et suspension/reprise des notes tenues. Le Prophet garde donc ses 5 voix tant qu'ECO + FX + drums suffisent. Si l'émulateur de l'utilisateur est vraiment > 70 % moyen, le plancher à 3 voix reste atteignable : le panneau debug montre alors VOICES PRO 3 et CAPS.

### (c) Dessin écran, scan touches, IRQ coupées
Plus long passage interruptions coupées mesuré : 1 169 instructions (5 microsecondes, fin dans isr_timer5). L'ISR audio rend chaque demi-bloc interruptions activées, le timer 10 kHz s'imbrique, la boucle UI est préemptée : dessin LCD et scan touches ne retardent pas le remplissage. Seul un coût ISR > un bloc cause un underrun (pointe mesurée à 77 % d'un bloc en u3r). Le gouverneur en dents de scie : filtré par moyenne sur 8 blocs, attente GOV_SETTLE/GOV_WAIT et refroidissement à pas croissant ; pas de basculement observé dans les runs.

### (d) Test objectif de glitch (clics du wav : spikes de dérivée seconde > 6x la moyenne locale + 700, trous, blocs répétés, frontières perdues ; `tools/loadtest.py`)
| cas | clics avant (2c2cab0) | clics après | trous |
|---|---|---|---|
| u3 (Vactrol + Prophet tenus + drums en séquence) | 15 | 248 | 0 |
| pk5 | n/d | 37 | 0 |
**Régression non résolue** : le nombre de clics de u3 monte (15 -> 248) parce que le gouverneur vole des voix drums (105 voix volées) dans ce motif extrême (toutes les lanes pleines). Contre-vérification : sans gouverneur ECO forcé = 14 clics ; sans ECO mais gouverné = 217 ; allonger le fondu de vol de 32 à 128 échantillons ne change rien (248), donc retiré. C'est le compromis choisi (pas de dépassement du bloc, plus de vol drums). Ce motif (6 lanes pleines à chaque pas) est pire que l'usage normal ; non vérifié à l'oreille. Piste restante : voler le Prophet en ECO avant les drums, ou baisser GOV_DMIN.
Pas de test ECO dédié (seulement les mesures du banc de charge) ; fxbank_test et state_test verts.

### DSEQ : lane agrandie supprimée
SELECT change la voix drum sans afficher le toast "LANE n" (qui masquait le séquenceur) ; toutes les lanes ont la même taille, la voix sélectionnée est marquée (cadre rouge + étiquette en gras). Captures : `shots/dseq-select-BEFORE-toast.png` (avant), `shots/dseq-lanes-AFTER.png` (après : BD -> SD -> CH -> CP -> CB).

## Mise à jour 4 : MAX TRACKS (réglage TEMPORAIRE de stabilité)

**Geste** : **HOME maintenu + PLAY** fait tourner 4 -> 3 -> 2 -> 1 -> 4 ; toast « MAX TRACKS n / TEMPORARY ». Affiché aussi dans le HUD debug (HOME + REC), dernière ligne, en rouge si < 4. Réglage **global, non sauvegardé** (ni preset, ni scène, aucun changement de format) ; défaut 4 à chaque démarrage. Captures : `shots/max-tracks-hud.png` (HUD à 4, puis toast 3, HUD à 3, toast 2, HUD à 2).

**Sémantique** : au plus N pistes rendent du son. Priorité : la **piste sélectionnée** toujours, puis les **numéros les plus bas** (choix simple et prévisible, pas de « dernière jouée »). Une piste hors ensemble n'est **pas rendue du tout** (moteur et effets : coût nul, `continue` avant le moteur) ; à la sortie / entrée de l'ensemble son niveau suit une rampe de un bloc (128 échantillons, ~3 ms). Changer de piste sélectionnée recalcule l'ensemble au bloc suivant. Les notes jouées sur une piste coupée sont **acceptées mais inaudibles** (elles ne réaffectent rien) ; l'arp et le séquenceur continuent d'avancer (horloge cohérente) ; un Prophet/drums coupé garde donc ses voix « actives » gelées et elles reprennent (avec rampe) au retour de la piste. La queue des effets d'une piste coupée est gelée, pas finie.

**Retrait** : tout est derrière `FEATURE_MAXTRACKS` (défini à 1 dans `src/fxstate.c`, mettre 0 ou supprimer les blocs). Lignes / fonctions touchées (grep `MAXTRACKS`) :
- `src/fxstate.c` l.11-21 : bloc `MAXTRACKS begin/end` (`FEATURE_MAXTRACKS`, `MAXTR_DEF`, `static uint8_t max_tracks`) ; l.151 : champ `hud.mt`.
- `src/voices.c` : bloc l.530-541 (`mt_prev`, `mt_mask()`) ; dans `voices_render` : 3 insertions `#if FEATURE_MAXTRACKS` (calcul de l'ensemble, `continue` si hors ensemble, rampe avant `fx_track`).
- `src/ctrl.c` l.382 : début de `ctrl_button` (HOME + PLAY, toast).
- `src/overlay.c` l.125 : dernière ligne de `draw_hud`.
- `tests/state_test.c` l.686 (test du cycle) ; `tools/loadtest.py` : variantes `mt1..mt4`, `mtchg`, `mthome`.

**Coût CPU** (fm1-emu, sans gouverneur, Vactrol + Minimoog (sélectionné, joué) + Prophet (accord 5 notes) + drums (toutes les lanes), effets par défaut, 6 s, bloc de 128 = 100 %) :
| N | pistes rendues | moyenne | pire bloc |
|---|---|---|---|
| 1 | Minimoog | 16,2 % | 16,5 % |
| 2 | + Vactrol | 24,4 % | 24,9 % |
| 3 | + Prophet | 50,6 % | 51,3 % |
| 4 | + drums | 81,3 % | 82,4 % |
(Sans gouverneur ; avec lui, le cas N=4 est ramené par les effets / ECO / drums comme en mise à jour 3.) Le Prophet à 5 voix et les drums pleins sont les deux gros postes : N=2 ou 3 retire l'un d'eux.

**Clics au changement de N pendant que ça joue** (`mtchg` : HOME + PLAY trois fois, 4 -> 3 -> 2 -> 1, tout en jouant) : 12 clics contre 13 pour le témoin sans PLAY (`mthome`), les plus gros pics sont identiques (1,98 s : la touche HOME elle-même, avec un bloc à 100 % dans ce test brut sans gouverneur à 81 %) : aucun clic ajouté par les rampes. Le test `mtchg` a 3 « trous » (fenêtres silencieuses) : ce sont les pistes coupées (silence voulu) et les notes ignorées pendant HOME maintenu, pas des pertes de blocs. Non testé : écoute humaine, silicium, émulateur de l'utilisateur. `state_test` (avec test 7e MAX TRACKS) et `fxbank_test` verts.

## Mise à jour 5 : cycles EDIT / GLO (Minimoog, Prophet) et knobs après SEQ

**Cycles** (`tab_press()`, `src/ctrl.c`) : sur Minimoog et Prophet, **EDIT répété** = pages de filtre : FILTER -> CONTOUR (Minimoog) / FILTER ENV (Prophet) -> FILTER ... (ce sont les deux seules pages de filtre que les moteurs possèdent ; les oscillateurs ne sont plus atteints par EDIT répété). **GLO répété** = MIXER -> OSCILLATOR 1 -> 2 -> 3 -> NOISE -> MIXER (Prophet : OSC A, OSC B, SUB OSCILLATOR, NOISE). Accords EDIT / GLO + OP1..4 (pages OSC), ENV, Vactrol et drums inchangés. Captures : `shots/cycle-edit-glo-minimoog.png`, `shots/cycle-edit-glo-prophet.png` (FILTER, CONTOUR, FILTER, puis MIXER, OSC1..3, NOISE, MIXER).

**Knobs noirs après SEQ sur le Vactrol : NON REPRODUIT, pas de correctif.** Le skin d'un potard n'est pas un état global : `draw_knob` / `knob_g` le recalculent à chaque dessin depuis `voices_engine(voices_sel())`, et `gfx_sprite` n'a pas d'état. Essayé en fm1-emu : ENV / FX -> SEQ -> ENV EDIT GLO LFO FX sur la piste Vactrol ; idem avec SEQ puis changement de piste vers Minimoog et retour ; SEQ + touche noire ; PLAY en marche avec notes tenues. Toutes les pages sont identiques avant et après SEQ. Nouveau contrôle automatique `tools/skin_check.py` (3 pistes : Vactrol, Minimoog, Prophet ; compare pixel à pixel la zone des potards de ENV / EDIT / GLO / LFO / FX avant et après SEQ) : OK, 0 pixel de différence. Non testé : changement de moteur via la liste ALGORITHM suivi de SEQ (la liste met 5 s à valider dans le script, je n'ai pas pu l'enchaîner), l'émulateur web de l'utilisateur et le LCD réel. Hypothèse restante, non vérifiée : un affichage partiel du LCD (mise à jour des lignes 124-206 seulement hors `dirty_all`) lu sur un émulateur lent. Si le défaut revient, relever la piste et le moteur affichés (HUD) et la séquence exacte de touches.
