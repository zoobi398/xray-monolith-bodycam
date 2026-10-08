# Cyclic gunfire in the engine (native voice for `custom_loop_sound` weapons)

> **English summary.** An opt-in native audio voice for the actor's weapons that carry `custom_loop_sound = true`
> (the TRUE CYCLIC sound system). The weapon code resolves the sample set (suppressed / subsonic / ADS / indoor, the same
> fallback cascade as the scripted system, read from the same `.ltx` keys) and hands every shot to a voice that owns its own
> OpenAL sources: each shot appends one block of the loop file to a queued source, starts are scheduled on the OpenAL device
> clock (`AL_SOFT_source_start_delay`), the end sample starts on a block boundary, the pitch follows the measured shot
> interval, and each shot also raises a `WPN_shoot` AI-hearing event (the scripted system never did). Console: `snd_cyclic_native`
> (0/1, default 0), `snd_cyclic_debug`, `snd_cyclic_pipeline_ms`. Lua: `cyclic_voice.ready()`, `cyclic_voice.set_indoor(bool)`.
> **Status: first cut, compiled, being tested in-game; off by default** -- with `snd_cyclic_native 0` the engine behaves exactly as
> before. The rest of this document is written in French (working notes).

---

# Cyclic gunfire dans le moteur — document de design (08)

Statut : **DESIGN uniquement, rien n'est codé.** Rédigé le 07/10/2026 à partir de la lecture du code de
`xray-monolith-bodycam_insurgency_recoil-2026.9.12-mt` (références `fichier:ligne` ci-dessous).
Sauvegarde exe avant ce chantier : `TRUE_CYCLIC_V2/_BACKUP_EXE_07.10.2026_before_cyclic_in_engine/` (ne pas toucher).
Contexte et historique : `TRUE_CYCLIC_V2/PLAN_ACTION_TEMP_.md`, `TRUE_CYCLIC_ENGINE_TEMP_.md`.

Règles de ce chantier (héritées des docs 01 à 07) :
- tout est **opt-in** : sans activation, comportement strictement identique à aujourd'hui ;
- aucun `.ltx` n'est modifié automatiquement ;
- un exe ancien doit continuer à fonctionner avec les scripts (repli sur le système Lua) ;
- compilation `engine-vs2022.sln /t:xrEngine /p:Configuration="DX11-AVX" /p:Platform="x64"` ;
- déploiement exe/pdb dans `BODYCAM_FADE_BASE/new_binaries_insurgency_recoil_v1/` et `F:\ANOMALY_forGAMMA_V095\bin\`.

---------------------------------------------------------------------------------------------------

## 1. Problème à résoudre

Le système actuel (`zzzzz_sound_loop_custom.script`, TRUE CYCLIC V1/V2) joue, pour l'acteur seulement :
`snd_shoot_start` (tir 1), puis `snd_shoot_loop` (un fichier de N blocs identiques en durée, démarré au tir 2),
puis `snd_shoot_end` (queue) au relâchement. Il vit entièrement côté Lua, donc **à la granularité d'une frame**
et sans accès à l'horloge audio. Conséquences mesurées :

1. **Cadence réelle ≠ cadence nominale.** `WeaponMagazined.cpp` (`state_Fire`, ~l.778-842) fait
   `fShotTimeCounter = fOneShotTime` (affectation) puis `-= dt`. L'intervalle réel vaut donc
   `(floor(T/dt)+1) × dt` : à 120 fps, 90 ms nominales donnent **11 frames = 91,67 ms** (mesuré par la sonde CADENCE).
   Le son (blocs de 90 ms) dérive de 1,67 ms par tir ; le fichier de boucle est « épuisé » avant la fin d'une longue
   rafale (d'où la marge 105/110 et la limite observée à 96-97 tirs).
2. **Coupure imprécise au relâchement.** La fin est décidée au prochain `actor_on_update` après un délai d'attente
   (cadence + marge) ; l'arrêt tombe au mieux à une frame près, et pas sur une frontière de bloc.
3. **Pas d'accès à l'horloge audio.** `snd:play(obj, delay, flags)` : le délai est décrémenté par frame
   (`starting_delay -= dt`, `SoundRender_Emitter_FSM.cpp:142`), le démarrage se fait au prochain `update` de l'émetteur.

Objectif : une **voix cyclique native** pour l'acteur, planifiée sur l'horloge audio, qui suit les tirs réels,
démarre/s'arrête sur des frontières de bloc au sample près, sans limite de durée de rafale.

---------------------------------------------------------------------------------------------------

## 2. Faits vérifiés dans le code (à la date du document)

### 2.1 Moteur audio (`src/xrSound`)
- Pas de thread audio dédié dans xrSound : `CSoundRender_Emitter::update(dt)` et le remplissage des buffers tournent
  une fois par frame (doc 06 : sur la tâche parallèle `seqFrameMT`, gatée par `mtSound`). Le **mixage** est fait par le
  thread interne d'OpenAL Soft (compilé dans `3rd party/OpenAL-new`).
- Lecture = **streaming** : `CSoundRender_TargetA::render()` remplit `sdef_target_count = 3` buffers de
  `sdef_target_block = 400 ms` (`SoundRender_TargetA.cpp:93-121`) ; `update()` (`:146-182`) dépile les buffers
  traités, en remplit un nouveau, et relance `alSourcePlay` si la file s'est vidée (sous-alimentation).
- Fin d'un son non bouclé : `fTimeToStop = fTime + longueur/vitesse` ; arrêt quand `fTime >= fTimeToStop`
  (`SoundRender_Emitter_FSM.cpp:149, 245`). Le pitch recalcule `fTimeToStop` (`SoundRender_TargetA.cpp:252-260`).
- Gain 2D : fondu par frame dans `update_culling()` branche `b2D` (doc 01) ; le gain n'est poussé à OpenAL que si
  l'écart dépasse 0,01 (`SoundRender_TargetA.cpp:213`).
- OpenAL Soft du moteur expose `alSourcePlayAtTimeSOFT` / `ALC_SOFT_device_clock` (démarrage planifié sur l'horloge
  du périphérique, au sample près). Il n'existe **pas** d'arrêt planifié natif.
- `alsoft.ini` (`F:\ANOMALY_forGAMMA_V095\bin`) : `frequency = 48000`, `period_size = 20`, `periods = 3`,
  `sample-type = int32`, `output-limiter` non défini (donc actif par défaut, cf. doc 06).

### 2.2 Comment les PNJ « entendent » un son (important)
Chaîne complète lue dans le code :
1. Un émetteur n'envoie un événement IA que si `owner_data->g_type != 0` et `owner_data->g_object != 0`
   (`SoundRender_Emitter.cpp:94-97`). Il le refait toutes les ~0,5 s (`s_f_def_event_pulse`, `SoundRender.h:22`) et
   au démarrage (`fTimeToPropagade = fTime`, `SoundRender_Emitter_FSM.cpp:150`).
2. Portée = `min(max_ai_distance, max_ai_distance × volume)` (`SoundRender_Emitter.cpp:101-102`).
   `max_ai_distance` vient du **commentaire OGG** du fichier (`SoundRender_Source_loader.cpp:131-138`, champ
   `AI_reaction distance`), copié dans l'émetteur au `start()` (`SoundRender_Emitter_StartStop.cpp:23`).
3. `IGame_Level::SoundEvent_Register` (`IGame_Level.cpp:382-445`) interroge les objets « réagissant aux sons »
   dans la boîte, calcule `Power = (1 - dist/max_ai_distance) × volume`, la multiplie par l'occlusion, et empile.
4. `SoundEvent_Dispatch` appelle `feel_sound_new(objet, g_type, ...)` → `CSoundMemoryManager::feel_sound_new`
   (`sound_memory_manager.cpp:132-214`) : la puissance est multipliée par un facteur **selon le type**
   (`m_weapon_factor` si `SOUND_TYPE_WEAPON`, etc.), comparée au seuil, et pour `SOUND_TYPE_WEAPON_SHOOTING`
   l'ennemi émetteur est ajouté à la mémoire de « hit » (`memory().hit().add`).
5. **D'où vient le type d'un son.** Un son est créé avec un `game_type` explicite ; le type du commentaire OGG
   n'est utilisé **que** si on demande `sg_SourceType` (-1) (`SoundRender_Core.cpp:352, 436`). Les sons d'armes sont
   chargés par le code de l'arme avec leur type : `m_eSoundShot = SOUND_TYPE_WEAPON_SHOOTING | eSoundType`
   (`WeaponMagazined.cpp:49, 108-174`). Donc pour une arme normale : **type = fixé par le code, portée IA = commentaire OGG**.
   Les commentaires OGG des moddeurs (distance min/max, volume de base, **AI_reaction distance**) fonctionnent bien ;
   seul le champ « sound type » du commentaire est ignoré pour les armes.

### 2.3 Constat sur le système Lua actuel — CONFIRMÉ EN JEU le 07/10/2026 (test T-IA)
Résultat de la sonde `[TC_AIHEAR]` (callback `npc_on_hear_callback`, un PNJ vivant, tirs du joueur) :
- `wpn_g3` (cyclique) : uniquement des événements `WPN_hit` (impacts de balles), **zéro `WPN_shoot`** ;
- `wpn_mp5` (non cyclique) : des `WPN_shoot` (3 à 5 par seconde) **en plus** des `WPN_hit`.
Le bruit des tirs des armes cycliques n'est donc **pas** perçu par les PNJ (le reste du paragraphe explique pourquoi).
Échantillon limité (une scène, un PNJ) mais net et conforme au code lu.

- `sound_object(path)` (utilisé aux lignes ~373 du master) appelle `CScriptSound(path, SOUND_TYPE_NO_SOUND)`
  (`script_sound.h:24`) : `g_type = 0` → `Event_Propagade` retourne aussitôt → **la boucle Lua n'alerte pas les PNJ**.
- Le tir natif de l'arme est supprimé (`result.volume_mult = 0` dans `on_before_play_hud_sound`), et le moteur ne joue
  pas un son dont le volume est ≤ `EPS_S` (`HudSound.cpp:449`) → aucun émetteur → aucun événement IA non plus.
- Conclusion probable : **les armes `custom_loop_sound` ne déclenchent pas les réactions IA liées au bruit du tir**
  (les PNJ peuvent réagir par d'autres voies : vue, impacts, balles, scripts). Si le test T-IA le confirme, la voix
  native corrige ce défaut ; en attendant, un contournement Lua existe (constructeur `sound_object(path, type)`,
  `WEAPON_SHOOTING = 0x80200000`), à n'essayer qu'avec ton accord.

### 2.4 Autres modifications moteur déjà en place (compatibilité)
- Doc 01 (fondu 2D `set_fade_in/out`) : la voix native peut réutiliser la même logique d'enveloppe.
- Doc 04/05 (occlusion) : ne touche que les émetteurs 3D ; la voix acteur est 2D (`occluder_volume = 1`).
- Doc 06 (ducking) : déclenché par `CActor::on_weapon_shot_start()` (C++), indépendant du son ; cible les voix PNJ 3D
  `occ_profile == 1`. **La voix native doit laisser ce déclenchement intact** (ne pas déplacer l'appel).
- Doc 03 (near-fade) : sons du monde uniquement. Doc 07 (jiggle) et doc 02 (recul) : indépendants du son.
- À relire en détail avant de coder : 03, 04, 05 et 07 (non relus en détail pour ce document).

---------------------------------------------------------------------------------------------------

## 3. Architecture proposée

Trois couches, avec une interface étroite entre elles :

```
Lua (rare : sur changement)         C++ xrGame (par tir)             C++ xrSound (voix)
-----------------------------       ---------------------------      --------------------------------
SAR / ADS / silencieux /            CWeaponMagazined::OnShot()  ---> CCyclicVoice::on_shot(t)
subsonique -> jeu de samples        FireEnd / reload / hide /   ---> CCyclicVoice::on_release()/abort(raison)
(start, loop, end, transitions)     mort / véhicule / enrayage       |  source OpenAL dédiée + file de blocs
        |                                                            |  démarrage planifié (horloge audio)
        +---- set_sample_set(...) ---------------------------------> |  alertes IA (type arme, portée OGG)
```

Principes :
- **Lua continue de résoudre le jeu de samples** (silencieux, subsonique, ADS, indoor, transitions) et les lit dans les
  `.ltx`. Il ne le pousse au moteur que sur changement. Ainsi SAR (Lua aujourd'hui, C++ peut-être plus tard) et
  le reste de la logique de sélection **ne sont pas dupliqués en C++**. Si SAR passe en C++ plus tard, seul le
  *appelant* de `set_sample_set` change.
- **Le moteur reçoit les événements d'arme directement** (pas de callback Lua par tir), donc l'enrayage n'est plus un
  problème d'ordre de callbacks : l'état de l'arme est lu dans le même appel que le tir.
- **Activation par arme** : le flag `custom_loop_sound = true` (déjà dans les `.ltx`) est lu en C++ dans `CWeapon::Load`.
  Sans le flag, ou avec un cvar global `snd_cyclic_native 0`, la voix native n'existe pas pour cette arme.
- **Un seul système actif par arme.** Le master Lua détecte la capacité du moteur (fonction exposée) et se met en retrait
  pour les armes prises en charge ; sur un exe sans la fonctionnalité, il reste le système actuel.
- PNJ : **inchangés** (leurs sons `snd_shoot*` passent par le chemin vanilla + `sar_snd_replacer`).

---------------------------------------------------------------------------------------------------

## 4. La voix : modèle audio

### 4.1 Pourquoi pas les émetteurs existants
Un émetteur xrSound démarre à la prochaine mise à jour de frame, s'arrête sur `fTimeToStop` à la frame près, et
streame par blocs de 400 ms. Impossible d'y obtenir un début/une fin au sample près. La voix native utilise donc
**sa propre source OpenAL**, hors machine d'état des émetteurs, avec des buffers statiques.

### 4.2 Buffers
- Au changement de jeu de samples, chaque OGG (start, loop, end, transitions) est décodé en PCM une fois et conservé
  (quelques centaines de Ko à ~1 Mo par fichier ; cache à taille bornée, vidé au changement de niveau).
- Convention du sound design (confirmée par l'auteur) : `snd_shoot_loop = path, shots, sample_rpm`. Les blocs durent
  `60 / sample_rpm` s (600 rpm = 100 ms, à ±1-2 échantillons à 44,1 kHz), chaque bloc commence **juste avant le pic
  d'attaque** de son tir, et le fichier se termine juste avant l'attaque d'un hypothétique tir `shots+1` : le **dernier
  bloc conserve donc la queue complète** du dernier tir (il est plus long que les autres). Le tir 1 est toujours
  `snd_shoot_start`, jamais dans la boucle. La durée de bloc est la même pour toutes les variantes (silencieux, indoor,
  subsonique, ADS) : seul le timbre change.
- Découpage : tranche `i` = `[i × B, (i+1) × B)` avec `B = round(sample_rate × 60 / sample_rpm)` ; la dernière tranche va
  jusqu'à la fin du fichier (queue). **Ne pas utiliser `samples/shots`** pour `B`, à cause de cette queue.
- Rafales plus longues que `shots` (chargeurs de 150-200) : après la dernière tranche *normale* on rebouclera sur une
  tranche intermédiaire (ou la première) en sautant la tranche-queue ; le choix de la tranche de rebouclage reste à fixer
  (point ouvert §11). Plus aucune « marge 105/110 » n'est nécessaire.

### 4.3 Déroulement d'une rafale
1. **Tir 1** : démarrage planifié de `snd_shoot_start` (déjà décalé d'une latence de pipeline constante `L`, §4.5).
2. **Tir 2 et suivants** : à chaque tir confirmé par l'arme, on met en file **une tranche** de boucle à la suite sur la
   même source. Les tranches sont contiguës : le rythme est fixé par le nombre d'échantillons, pas par la frame.
   `snd_shoot_start` est coupé (fondu) à l'entrée de la boucle comme aujourd'hui (réglages `*_cutoff_*` conservés).
3. **Relâchement** : l'arme sait *exactement* si un tir est encore à venir (`FireEnd` annule le tir en attente). La
   dernière tranche mise en file est donc la dernière : la source s'arrête **toute seule** à la frontière (file vide), sans
   commande d'arrêt. `snd_shoot_end` est planifié sur l'horloge audio au même instant de frontière (source dédiée), avec
   le fondu d'entrée existant (`snd_shoot_end_fadein*`).
4. **Cas d'arrêt** (politique actuelle conservée) : boucle en cours → fin avec queue ; phase START → arrêt silencieux ;
   enrayage → fin immédiate ; rechargement, arme rangée/lâchée/déplacée, véhicule, mort, destruction de l'acteur.
   Un arrêt immédiat = fondu court sur la source (les fondus par frame existent déjà) puis `alSourceStop`.

Point technique à retenir : OpenAL ne permet **pas de retirer** des buffers déjà en file, seulement d'arrêter la source.
D'où la règle « on ne met en file que ce qui est confirmé » (pas d'anticipation de plusieurs blocs), compensée par la
latence de pipeline `L` ci-dessous.

### 4.4 Suivre la cadence réelle (le point délicat)
Si les tranches sont lues à leur durée nominale (90 ms) alors que les tirs arrivent toutes les 91,67 ms, le son prend de
l'avance de 1,67 ms par tir. Stratégies :
- **S1 — pitch asservi (recommandée par défaut).** On mesure l'intervalle moyen des tirs (fenêtre glissante) et on règle
  `AL_PITCH = nominal / mesuré` (≈ 0,982), borné à ±3 %. Pas de silence inséré, pas de coupure ; le timbre bouge de
  ~0,3 demi-ton, inaudible sur un tir. Si la correction de cadence moteur (S3) est active, le pitch revient à 1,000.
- **S2 — micro-silences.** On insère `intervalle_réel - nominal` de silence entre tranches. Pas de changement de timbre mais
  la queue de réverbération incluse dans la tranche est coupée net sur 1-2 ms (clic possible → fondu de 0,5 ms).
- **S3 — corriger la cadence dans l'arme** (§6) pour que réel = nominal. Meilleur pour l'audio, mais change la
  cadence réelle de tir.

### 4.4bis Changement de cadence par les upgrades (comportement à conserver)
Aujourd'hui (`zzzzz_sound_loop_custom.script:576-585, 760-762, 898-900`) : la cadence réelle de l'arme (modifiée par les
upgrades/le moteur) est lue, puis `fréquence = rpm_réel / sample_rpm` est appliquée à la boucle (`snd.frequency`). Une arme
mixée à 600 rpm qui passe à 700 rpm joue donc sa boucle 1,167× plus vite **et** plus aiguë : c'est ce que tu as remarqué.
La voix native garde exactement ce principe, avec deux étages :
1. **Pitch de base** = `rpm_réel / sample_rpm` (aucun plafond de ±3 %, seulement les bornes d'OpenAL), appliqué à la source
   de la boucle ET dimensionnant la durée des tranches (`B / pitch`) ; fixé au début de la rafale et à chaque changement
   de rpm de l'arme.
2. **Asservissement fin** (§4.4, S1) = petite correction autour de ce pitch (±3 %) pour absorber l'écart entre cadence
   théorique et cadence mesurée (le défaut de 91,67 ms).
Donc **un seul export par variante suffit, quel que soit le rpm** ; aucun nouvel export à faire. Limite connue (identique à
aujourd'hui) : changer la vitesse change aussi la hauteur ; un étirement temporel sans changement de hauteur
n'est pas prévu (algorithme lourd, résultat discutable sur des tirs).

### 4.5 Latence de pipeline `L`
Un tir est détecté à une frame donnée ; la tranche suivante doit être en file *avant* la fin de la précédente. Avec un
intervalle de 91,67 ms contre 90 ms de tranche, il faut `L > 1,67 ms + gigue de frame` (≈ 2 frames à 120 fps, soit
~17 ms ; à 60 fps ~33 ms). `L` est constante, donc le rythme reste régulier ; elle s'ajoute au décalage déjà présent
dans le fichier (« prefire » jusqu'à 45 ms dans `snd_shoot_start`). **À mesurer** : l'ensemble doit rester sous les ~50 ms
de perception de simultanéité. Une sous-alimentation (frame très longue) est rattrapée par `alSourcePlayAtTimeSOFT`
sur la tranche suivante, avec un seul trou audible plutôt qu'une rafale coupée.

### 4.6 Horloge
Chaque frame, on lit en même temps l'horloge de jeu du son (`SoundRender->fTimer_Value`) et l'horloge du périphérique
(`ALC_DEVICE_CLOCK_SOFT`), pour convertir « temps de tir » en « instant de démarrage planifié ». L'erreur de conversion
est de l'ordre de la microseconde.

### 4.7 Threading — niveaux
- **Niveau 2 (retenu pour le premier jet)** : toutes les commandes OpenAL sont émises depuis le code de mise à jour du
  son (déjà sur la tâche parallèle `seqFrameMT`) ; les événements de l'arme (thread de jeu) passent par une petite file
  protégée. Le *mixage* reste dans le thread OpenAL.
- **Niveau 3 (thread d'ordonnancement dédié)** : à ne faire que si un test de charge (gigues de frame simulées) montre des
  artefacts. Utilité limitée : les événements de tir naissent de toute façon dans le thread de jeu, un thread ne rattrape
  donc pas un tir arrivé en retard ; il n'aide que pour le travail d'ordonnancement lui-même.

---------------------------------------------------------------------------------------------------

## 5. Alertes IA (réactions des PNJ)

La voix native doit émettre elle-même les événements IA (la source dédiée n'est pas un émetteur xrSound) :
- À chaque tir confirmé (équivalent vanilla : un événement par son d'arme), via la même file `s_events` /
  `SoundEvent_Register`, avec un `ref_sound_data` portant `g_object = acteur` et
  `g_type = m_eSoundShot` **de l'arme** (même sous-type que vanilla : `WEAPON_SHOOTING | type d'arme`).
- Portée : `max_ai_distance` du **commentaire OGG de l'échantillon actif** (start/loop selon la phase, jeu silencieux /
  subsonique pris en compte puisque chaque jeu a ses propres commentaires) × volume.
- Position : acteur/écouteur (le son 2D utilise `listener_position()` comme position, `IGame_Level.cpp:398-401`).
- `get_occlusion_to` est appliqué comme pour tout son (inchangé).

---------------------------------------------------------------------------------------------------

## 6. Correction de cadence (`WeaponMagazined`, optionnelle)

`WeaponMagazined` est la classe C++ qui implémente le tir des armes à chargeur (fusils, pistolets-mitrailleurs…). Sa
fonction `state_Fire` décide, frame par frame, quand part le prochain tir à l'aide d'un compteur :
`fShotTimeCounter = fOneShotTime` puis `-= dt`. Ce compteur est *remis* à la durée entre deux tirs (au lieu d'y *ajouter*),
si bien que le temps « déjà écoulé au-delà » est perdu à chaque tir : la cadence réelle est toujours un peu plus lente que
celle du `.ltx` (voir mesure §1), et elle dépend du FPS.

Options :
- **A. `+=` dans le moteur** (cvar opt-in) : cadence réelle = nominale en moyenne. Effet de bord : la cadence réelle monte
  (~+1,8 % à 120 fps, plus à 60 fps), donc les DPS, le recul par seconde et l'animation changent légèrement. Même défaut
  présent dans `WeaponSSRS.cpp` ; `CarWeapon`/`HelicopterWeapon` utilisent déjà `+=`.
- **B. Audio suit la cadence mesurée** (stratégie S1 du §4.4) : aucun changement de gameplay. **C'est le choix du premier
  jet** : la voix native n'a pas besoin de A pour être juste.
- A reste possible plus tard, derrière un cvar, si tu veux aligner la cadence réelle sur le `.ltx`.

---------------------------------------------------------------------------------------------------

## 7. Configuration

- **Aucun `.ltx` modifié.** Lua lit déjà `snd_shoot_start`, `snd_shoot_loop = path, shots, sample_rpm`, `snd_shoot_end`, les
  variantes (`_suppressed`, `_subsonic`, `_ads`, `_indoor`), les transitions et les fondus ; il pousse le résultat au moteur.
- Clés qui deviendront **inutiles** avec la voix native (conservées et ignorées au début ; bilan à faire après le premier
  jet, tu décideras alors de ce qu'on commente) : `snd_shoot_loop_delay`, `snd_shoot_end_cycle_margin`,
  `snd_shoot_start_timeout_tolerance` (et leurs variantes `_suppressed`), et la marge « 105 au lieu de 100 » dans `shots`.
- Nouveaux réglages moteur (propositions) : `snd_cyclic_native` (0/1, défaut 0), `snd_cyclic_pipeline_ms` (latence `L`,
  défaut calculé), `snd_cyclic_debug` (journal des événements).
- Une page MCM « Cyclic gunfire (engine) » suivra le schéma des pages existantes (options_modded_exes_*), sans oublier
  le `GROUP` de `options_modded_exes_gameplay.script` (leçon du doc 05).

---------------------------------------------------------------------------------------------------

## 8. Compatibilité avec OpenAL, limiteur et ducking

- La source dédiée envoie dans le **même contexte OpenAL** : le limiteur de sortie reste actif et s'applique à tout le mix,
  comme aujourd'hui ; le ducking (doc 06) continue de baisser les voix PNJ quand l'acteur tire, car son déclencheur est
  `on_weapon_shot_start`, pas le son.
- Effets (EFX/réverb SAR) : la voix acteur est 2D et n'est pas occultée ; elle peut être envoyée au slot de réverb
  comme les autres sons 2D de l'acteur selon le comportement actuel (`Slot` dans `render()`), à reproduire à l'identique.
- Pause du jeu / menus : la voix doit se mettre en pause avec les autres sons (`iPaused`).

---------------------------------------------------------------------------------------------------

## 9. Plan de test (à écrire dans l'ordre)

- **T-IA (avant tout code)** : l'affichage `snd_stats_ai_dist` n'est compilé que dans les builds DEBUG
  (`Stats.cpp:531`, `#ifdef DEBUG`) et ne dessine que les sons 3D ; il est donc **inutilisable** avec l'exe release.
  À la place, le script temporaire `DIAG_TEMP_/gamedata/scripts/zzzzzzzz_tc_aihear_probe_temp_.script` écoute le callback
  `npc_on_hear_callback` et affiche une ligne par seconde `[TC_AIHEAR] ... wpn=... cyclic=... events=... shooting=...`
  pour les événements dont l'émetteur est le joueur. Tirer avec une arme cyclique puis avec une arme normale à portée
  d'un PNJ vivant et comparer `events`/`shooting`. (Le callback est émis par un script du jeu que je n'ai pas pu relire ;
  s'il ne se déclenche jamais, aucune ligne n'apparaît : cela ne prouverait rien, il faudrait alors un compteur moteur.)
- **T-cadence** : réutiliser la sonde CADENCE du diag pour comparer l'intervalle réel des tirs et celui des tranches.
- **T-coupure** : enregistrer la sortie audio (loopback) et mesurer l'écart entre le dernier tir et la fin de la boucle,
  V2 Lua contre voix native, sur 20 rafales.
- **T-rafale longue** : chargeur de 200+ coups (M60) : pas de trou, pas de limite à 96-97 tirs.
- **T-arrêts** : rechargement, rangement, véhicule, mort, enrayage, changement d'arme pendant le tir.
- **T-charge** : 15-20 PNJ qui tirent + rafale acteur ; mesurer le coût (profiler) et les à-coups de frame.
- **T-compat** : ducking, occlusion, limiteur, SAR indoor/outdoor, ADS, silencieux, subsonique.

## 10. Phases d'implémentation

1. Interfaces (`CSound_manager_interface`) + lecture du flag + cvars + bascule sur le chemin existant (rien ne change).
2. Voix (source dédiée, buffers statiques, file de tranches, démarrage planifié) sur le thread principal.
3. Alertes IA.
4. Cas d'arrêt et enrayage.
5. Asservissement du pitch (cadence mesurée).
6. Page MCM + documentation finale ; bilan des clés `.ltx` devenues inutiles.
7. (Si nécessaire) thread d'ordonnancement.

## 11. Risques et décisions ouvertes

- **Tranche de rebouclage** pour les rafales plus longues que `shots` (§4.2).
- **Latence `L`** (§4.5) : valeur à fixer par mesure, avec le prefire du `snd_shoot_start`.
- **Alerte IA** : confirmer le constat §2.3 en jeu avant d'écrire la partie §5.
- **Cache PCM** : taille maximale et politique d'éviction.
- **Exe ancien** : le master Lua doit tester la capacité moteur ; prévoir une fonction exposée (`has_cyclic_native()`).
- **Course de données** entre thread de jeu et tâche son : file protégée obligatoire pour les événements d'arme.
- **Armes sans le flag** : jamais touchées ; toute la logique est gatée par `custom_loop_sound` + `snd_cyclic_native`.
- **SAR en C++ (phase 3)** : ne demande pas de modifier la voix, seulement de changer qui appelle `set_sample_set`.

---------------------------------------------------------------------------------------------------

## 12. État d'implémentation — premier jet (08/10/2026)

Statut : **compilé (0 erreur), déployé, PAS ENCORE TESTÉ EN JEU.** Sans `snd_cyclic_native 1`, l'exe se comporte
exactement comme avant (toutes les nouvelles commandes sont à 0 par défaut). Sauvegarde de l'exe précédent :
`TRUE_CYCLIC_V2/_BACKUP_EXE_07.10.2026_before_cyclic_in_engine/` (identique à l'exe qui était déployé le 07/10).

### Ce qui a été fait (diffère un peu du plan §3 sur un point : voir « Résolution des samples »)

| Fichier | Rôle |
|---|---|
| `xrSound/Sound.h` | `SCyclicEvent` (événement arme → voix), `ECyclicEvent`, `sound_event_raw`, cvars, 3 méthodes d'interface (`cyclic_native_ready`, `cyclic_event`, `set_handler_raw`) |
| `xrSound/SoundRender_CyclicVoice.h/.cpp` (nouveaux) | la voix : 16 sources OpenAL propres, PCM décodé en mémoire (cache 96 Mo), file de tranches, démarrages planifiés sur l'horloge du périphérique (`alSourcePlayAtTimeSOFT`), asservissement du pitch, fondus, fin sur frontière de bloc |
| `xrSound/SoundRender_Core.*`, `_Core_Processor.cpp`, `_CoreA.*` | branchement : `cyclic_update` par frame, pause, arrêt, init avant la création des targets (pour ne pas manquer de sources), événements IA |
| `xrEngine/IGame_Level.*` | `SoundEvent_RegisterRaw` : alerte IA identique à celle d'un tir vanilla (portée = commentaire OGG) |
| `xrEngine/xr_ioc_cmd.cpp` | `snd_cyclic_native` (0/1, défaut 0), `snd_cyclic_debug` (0/1), `snd_cyclic_pipeline_ms` (défaut 20) |
| `xrGame/CyclicGunfire.h/.cpp` (nouveaux) | lecture des `.ltx` (mêmes clés que le master Lua), cascade silencieux / subsonique / ADS / indoor identique à `build_chain`, transitions indoor↔outdoor et ADS, fondus, délais |
| `xrGame/Weapon.h`, `WeaponFire.cpp`, `WeaponMagazined.*` | drapeau `custom_loop_sound` lu en C++ ; événement par tir (`CyclicNativeShot`, avant le son vanilla qui est alors sauté) ; fin de rafale sur `FireEnd` ; arrêt sur mort / arme plus active |
| `xrGame/script_sound_script.cpp` | Lua : `cyclic_voice.ready()`, `cyclic_voice.set_indoor(bool)`, `cyclic_voice.indoor()` |

**Résolution des samples.** Le plan §3 prévoyait que Lua pousse le jeu de samples. Pour respecter « le moins d'allers-retours
possible », la résolution est faite **entièrement en C++** (port de `load_section_config` / `build_chain` / `resolve`). Le
seul échange restant avec Lua est `cyclic_voice.set_indoor()`, poussé par `sar_main` **uniquement quand l'état indoor
bascule** (la détection indoor de SAR reste en Lua jusqu'à la phase 3).

**Côté Lua (fichiers du projet, à installer comme les autres) :**
- `zzzzz_sound_loop_custom.script` : si `cyclic_voice.ready()` le master ne joue plus rien (il garde seulement l'effet de
  gameplay « munition subsonique dans une arme incompatible → enrayage ») ; sans moteur compatible ou avec
  `snd_cyclic_native 0`, il fonctionne exactement comme avant ;
- `sar_main.script` : pousse l'état indoor au moteur (même hystérésis que `zzzz_ads_sound._G_isIndoor`).

### Comportements voulus / choix faits
- Fin de rafale : sur `FireEnd` (relâchement, rechargement, rangement, arme lâchée) ou, pour une rafale limitée ou le
  dernier coup du chargeur, **dès le dernier tir** (l'arme sait qu'aucun tir ne suivra). L'échantillon de fin démarre à
  `frontière du dernier bloc − snd_shoot_end_cycle_margin`.
- Rafales plus longues que `shots` : rebouclage sur les blocs `0..shots-2` (la tranche-queue n'est utilisée qu'au bout).
- Bascule indoor/ADS : le bloc suivant provient du nouveau fichier **à l'indice de tir courant** (le script Lua repartait du
  début du fichier, faute de pouvoir se placer au milieu).
- Enrayage vanilla (`bMisfire`) : coupe la boucle immédiatement. Enrayage WPO : l'arme cesse de tirer (`FireEnd`), la fin
  est naturelle, comme en V1 (aucun appel Lua nécessaire).
- Alertes IA : un événement `WPN_shoot` par tir, portée = `AI_reaction distance` de l'OGG de l'échantillon joué.

### Limites connues du premier jet
- Fondu de coupure du `snd_shoot_start` et fondus fin/boucle : à la granularité d'une frame (comme les fondus moteur).
- Un échantillon de transition mixé à un autre `sample_rpm` que la boucle garde le pitch de la boucle jusqu'au bloc suivant.
- Aucune mesure encore : latence `snd_cyclic_pipeline_ms`, gain de l'asservissement du pitch, comportement sous charge.
- `all_subsonic` : toutes les cartouches du chargeur doivent être `is_subsonic` (équivalent du « chargeur uniforme » du script).

### Tester (ordre conseillé)
1. Installer les 2 scripts Lua du projet (master + `sar_main`) dans les mods V2.
2. En jeu, console : `snd_cyclic_native 1` puis `snd_cyclic_debug 1`. (Retour arrière immédiat : `snd_cyclic_native 0`.)
3. Tirer une arme cyclique (G3) : rafale courte, rafale longue, relâchement ; chercher les lignes `* [cyclic]` du log.
4. Sonde `[TC_AIHEAR]` : on doit maintenant voir des `WPN_shoot` avec l'arme cyclique.
5. Ensuite seulement : indoor/outdoor, ADS, silencieux, rechargement en tirant, mort.

### 12bis. Correctifs après le premier log de test (08/10/2026)
Constat dans le log (lignes `* [cyclic]`) : avec `snd_cyclic_pipeline_ms 100` ou `20` en conditions normales, le suivi est
propre (pitch ≈ 0,982 = 90 ms / 91,7 ms, file d'avance à la cible). Trois défauts apparaissent quand le jeu est irrégulier
(pic de lag, centaines de PNJ) ou avec `snd_cyclic_pipeline_ms 0` : un « underrun » à presque chaque tir et un pitch qui
plonge au plancher (×0,90).
Causes trouvées : (1) l'horloge du périphérique OpenAL n'avance que par pas de ~10 ms, donc les temps de tir mesurés
étaient flous (intervalles de 90 ou 100 ms au lieu de 91,7) ; (2) un intervalle isolé très long (lag) entrait tel quel dans
la moyenne et faisait chuter le pitch ; (3) à 0 ms de délai aucune marge ne couvre la gigue de traitement d'une frame.
Corrigé dans `SoundRender_CyclicVoice.cpp` : horloge affinée (minuteur haute résolution + décalage maximal sur 3 s), médiane de 5
intervalles puis moyenne lente, délai effectif = `max(snd_cyclic_pipeline_ms, 1,3 × durée moyenne d'une frame)` (journalisé au
`begin`). `snd_cyclic_pipeline_ms` devient donc un MINIMUM.
