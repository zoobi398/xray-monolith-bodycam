# HUD animation layers (independent additive animation layers for weapons)

> **English summary.** Opt-in per weapon (`hud_layers` key of the HUD section): a layer is a motion of the weapon's own OMF played
> on an additive animation channel (2 or 3) of the weapon model, started by an event (`shot`, `burst_start`, `fire_end`,
> `anim:<HUD animation name or pattern*>`, `mark:<motion mark>`, or Lua `hud_layers.play(section)`), with its own duration and a
> retrigger policy (`crossfade` / `restart` / `ignore`). Channels 2 and 3 are never cut by the main animation (channel 0), so a belt
> or a carry handle can keep moving after the shot animation ended. Weapons without `hud_layers` are untouched. Console:
> `g_hudlayers_debug`. **Status: implemented, compiled, validated at engine level in-game** (triggering, blend creation, instance
> cap); visual authoring workflow described below (Blender `io_scene_xray` export, OMF Editor for motion marks).
> The rest of this document is written in French (animator guide).

---

# Théorie : calques d'animation indépendants pour les armes HUD (X-Ray / Anomaly)

Statut : **implémenté (premier jet, 08/10/2026), compilé, déployé, PAS ENCORE TESTÉ EN JEU** — voir la section 11 (mode d'emploi, configuration,
explication du code). Les sections 1 à 10 restent la théorie et le raisonnement. Les vérifications ont été faites dans le code
d'un build basé sur `xray-monolith` (Anomaly 1.5.x, moteur modifié). Les numéros de ligne cités
peuvent varier d'un build à l'autre, mais les mécanismes décrits viennent du moteur X-Ray de base
(Clear Sky / Call of Pripyat) et sont présents sur tous les forks courants.

Public visé : un animateur qui veut savoir **ce qui est possible**, **ce qu'il doit produire**, et
**ce qu'un programmeur devra ajouter** au moteur.

---

## 1. Le problème

En vue à la première personne, une animation d'arme est jouée sur deux modèles à la fois :

- les **mains** (`hands`, OMF des bras) ;
- l'**arme** (`item_visual`, OMF de l'arme : culasse, levier d'armement, bande, etc.).

L'animation de l'arme est jouée **en même temps que celle des mains et avec la même durée**. Si
`anm_shoot` fait 30 frames, tous les bones de l'arme s'arrêtent à la frame 30. Dès qu'une nouvelle
animation démarre (idle, tir suivant, sprint...), elle **remplace entièrement** la précédente sur
tous les bones de l'arme.

Conséquences :

- une bande de mitrailleuse (LMG) riggée avec des bones de balles ne peut pas continuer à osciller
  après la fin de l'anim de tir (inertie, retard, amortissement) ;
- un carry handle, une sangle ou un levier ne peut pas avoir une animation qui démarre sur un
  événement et **va jusqu'au bout de sa propre durée** sans être coupée par l'anim suivante.

Ce qu'on veut : une **couche d'animation indépendante**, déclenchée par un événement, avec sa propre
durée, qui **s'ajoute** à l'animation principale sans la remplacer et sans que les autres armes du jeu
soient affectées.

---

## 2. Comment X-Ray mélange les animations (version simplifiée)

### 2.1 Les briques

- **Motion** : une animation dans un OMF (par exemple `anm_shoot`, `idle`).
- **Bone part / partition** : un groupe de bones défini dans l'OMF (4 maximum). Une motion peut être
  jouée sur une seule partition.
- **Blend** : une instance d'animation en cours (motion + temps courant + poids). Plusieurs blends
  peuvent tourner en même temps sur un même bone ; le moteur les mélange.
- **Canal (channel)** : chaque blend appartient à un canal. **Il y a 4 canaux (0 à 3)** par modèle.

### 2.2 Les canaux : la clé de tout

Le moteur a des règles de mélange fixes par canal (`Layers/xrRender/Animation.cpp`) :

| Canal | Mélange avec les canaux précédents | Usage dans le jeu de base |
|---|---|---|
| 0 | base | toutes les animations normales (HUD compris) |
| 1 | interpolation | quasiment pas utilisé |
| **2** | **additif** | réactions aux impacts des PNJ |
| **3** | **additif** | réactions aux impacts des PNJ |

Trois propriétés, vérifiées dans le code, rendent les canaux 2 et 3 parfaits pour notre besoin :

1. **Indépendance.** Lancer une animation sur le canal 0 (ce que fait toujours le HUD) ne coupe et ne
   fait disparaître en fondu **que les blends du canal 0**. Une animation lancée sur le canal 2 ou 3
   continue donc jusqu'au bout, quoi que fasse l'animation principale.
   (`CKinematicsAnimated::LL_PlayCycle` : le fondu et la fermeture ciblent seulement `1 << channel`.)

2. **Additif.** Sur ces canaux, le moteur prend la pose du calque à l'instant `t`, **retire la pose de
   sa frame 0**, et ajoute la différence obtenue à la pose produite par le canal 0 :

   ```
   pose_finale(bone) = pose_canal_0(bone)  ⊕  [ calque(t) ⊖ calque(frame 0) ] × facteur_canal
   ```

   Un bone qui ne bouge pas dans l'anim du calque reçoit une différence nulle : il n'est pas touché.

3. **Fin propre automatique.** Une anim non bouclée (« stop at end ») jouée sur le canal 2 ou 3 passe
   automatiquement en fondu à la fin puis est détruite (`fall_at_end = stop_at_end && channel > 1`
   dans `IBlendSetup`). Elle ne reste pas en mémoire.

Le calcul final des bones évalue déjà les 4 canaux (`CKinematics::Bone_Calculate`, masque `u8(-1)`).

### 2.3 Ce qui manque

> **Le mélange existe dans le moteur, mais aucune ligne du code de jeu ne l'utilise pour les armes HUD.**

- `attachable_hud_item::anim_play` (`xrGame/player_hud.cpp`) joue toujours l'anim de l'arme sur
  **toutes les partitions, canal 0**.
- Il n'existe **aucune clé `.ltx`** pour demander « joue telle motion sur le canal 2 ».
- Il n'existe **aucune fonction Lua** pour le faire non plus. Les fonctions existantes
  (`play_hud_motion`, `play_cycle`, `script_attachment:play_motion`) jouent toutes sur le canal 0.

**Donc : il faut du code C++ dans `xrGame` et une recompilation du moteur.** C'est un ajout modeste
(un petit contrôleur et quelques points d'accroche, voir §6). En revanche **le rendu (`xrRender`) n'a
pas besoin d'être modifié** : les fonctions nécessaires (`LL_PlayCycle(..., channel)`,
`LL_SetChannelFactor`) sont déjà disponibles via l'interface `IKinematicsAnimated`.

---

## 3. Le principe proposé : « calques d'animation HUD »

Une arme peut déclarer, dans sa section HUD, une liste de **calques**. Un calque, c'est :

- une **motion** de l'OMF de l'arme ;
- un **canal** additif (2 ou 3) ;
- un **déclencheur** : le tir, la fin d'une rafale, une marque d'event dans une anim, un appel Lua...
- une **règle de relance** : que faire si le déclencheur revient alors que le calque tourne encore.

Exemple de configuration (syntaxe proposée, **n'existe pas encore**) :

```ini
![wpn_pkm_hud]
hud_layers = belt_shot, belt_settle

[belt_shot]
motion    = belt_feed_cycle     ; une avance de maillon
channel   = 3
trigger   = shot                ; à chaque balle tirée
retrigger = crossfade
speed_rpm = true                ; vitesse calée sur la cadence de l'arme

[belt_settle]
motion    = belt_settle         ; la bande qui ballotte puis se calme (1,5 s par exemple)
channel   = 2
trigger   = fire_end            ; à la fin de la rafale
retrigger = crossfade
```

Une arme **sans** clé `hud_layers` se comporte exactement comme aujourd'hui, sans aucun coût en plus.
**Rien n'est rétroactif** : on n'a pas besoin de réanimer les armes existantes.

---

## 4. Guide pour l'animateur

### 4.1 Règles pour une anim de calque

1. **Elle est dans l'OMF de l'arme** (`item_visual`), pas dans celui des mains. C'est une motion
   normale (cycle), **pas une FX**.
2. **La frame 0 est la pose de repos.** Le moteur soustrait la frame 0, donc tout ce qui est à la
   frame 0 ne compte pas. Le plus simple est de mettre à la frame 0 la même pose que celle des bones
   concernés dans l'idle.
3. **La dernière frame revient à la frame 0.** Le fondu de sortie n'adoucit pas visuellement un calque
   seul (voir §7). Si la dernière frame n'est pas au repos, on verra un saut à la fin.
4. **N'anime que les bones utiles.** Les autres bones doivent rester **parfaitement immobiles** dans
   cette motion : leur différence vaut alors zéro et ils ne sont pas touchés.
5. **Coche « stop at end » (pas de boucle).** C'est ce qui active la fin propre automatique sur les
   canaux 2 et 3.
6. **Garde des amplitudes raisonnables.** L'addition de rotations est fiable pour des oscillations,
   des petits débattements et des secousses. Pour une rotation de 90° ou plus sur une chaîne de
   bones, il faut tester en jeu, car l'ordre des rotations peut décaler un peu le résultat.
7. **Nomme-la clairement** (par exemple `belt_settle`, `handle_drop`) pour qu'on la trouve dans la
   config.

### 4.2 Optionnel : un bone part dédié

Si tu définis un bone part qui ne contient que les bones de la bande (ou du carry handle), le calque
peut n'être calculé que sur ces bones. Ce n'est pas obligatoire (la différence est nulle sur les autres
bones) mais c'est un peu moins coûteux. Les anims principales de l'arme continuent de fonctionner : le
HUD les joue de toute façon sur toutes les partitions.

### 4.3 Ce qu'un calque peut et ne peut pas faire

- ✅ Ajouter un mouvement par-dessus n'importe quelle anim principale (idle, tir, sprint, inspect...).
- ✅ Durer plus longtemps que l'anim qui l'a déclenché, sans être coupé.
- ✅ Se superposer à une anim principale qui bouge **déjà** la bande : les deux s'additionnent.
- ❌ Bouger les **mains**. Le calque ne touche que le modèle de l'arme. Si une main tient la bande
  (pendant un reload par exemple), la bande peut bouger sans que la main suive. Dans ce cas, ne pas
  déclencher le calque pendant ce reload, ou garder une amplitude très faible.
- ❌ Réagir aux mouvements du joueur en temps réel (souris, saut...). Ça, c'est le rôle du procédural
  (§8).

---

## 5. Cas concret : la bande de LMG

### 5.1 Le faux problème du « n-1 qui devient n »

On pourrait croire qu'à chaque tir il faut décaler l'identité des balles (la balle 5 devient la
balle 4...). **Ce n'est pas nécessaire.** La technique utilisée dans la plupart des jeux est le
**cycle d'avance d'un maillon** :

- l'anim dure exactement le temps entre deux tirs ;
- pendant ce temps, **chaque bone de balle va à la position de la balle suivante** sur le trajet de
  la bande ;
- à la fin, l'anim revient d'un coup à la frame 0.

Comme tous les maillons sont identiques, ce retour ne se voit pas : on a l'impression que la bande
avance en continu. Aucun réindexage ni bookkeeping.

Pour suivre la cadence réelle de l'arme (y compris les upgrades de cadence), le code joue l'anim à la
vitesse :

```
vitesse = durée_de_l_anim_en_secondes × (RPM / 60)
```

Exemple : une anim de 4 frames (30 fps, donc 0,133 s) sur une arme à 750 coups/min (12,5 coups/s)
donne une vitesse de 0,133 × 12,5 ≈ 1,67.

La disparition des balles quand il reste peu de munitions est gérée à part par le système existant
(`HUD_VisualBulletUpdate`). Ce n'est pas le rôle du calque.

### 5.2 L'inertie et le ballottement

Un second calque `belt_settle`, déclenché à **la fin de la rafale** (et/ou à chaque tir), contient
l'oscillation amortie de la bande : la bande continue sur son élan, revient, dépasse un peu, puis se
stabilise.

- La durée est libre (1 à 2 s par exemple), sans lien avec `anm_shoot` ni avec l'idle.
- Dans Blender, l'animateur peut utiliser une IK ou une contrainte à gradient pour **produire** le
  mouvement, puis **le baker** dans les bones des balles à l'export. Le jeu ne lit que les clés
  bakées.

### 5.3 Combinaison

```
canal 0 : anm_shoot / idle / ...   (comme aujourd'hui, inchangé)
canal 3 : belt_feed_cycle          (relancé à chaque balle, vitesse = cadence)
canal 2 : belt_settle              (lancé à la fin de la rafale, va jusqu'au bout)
```

---

## 6. Ce que le programmeur doit ajouter (côté `xrGame`)

Ordre de grandeur : **150 à 250 lignes de C++**, plus une recompilation. Sur le build de référence, le
système « gun part jiggle » (`weapon_part_jiggle.cpp`) peut servir de modèle de structure : il
s'accroche à `attachable_hud_item` de la même façon.

### 6.1 Un contrôleur par arme HUD

`CHudAnimLayers`, membre de `attachable_hud_item` :

- `install()`, appelé à la fin de `attachable_hud_item::load()` : lit `hud_layers` et résout chaque
  motion avec `ID_Cycle_Safe` sur le modèle de l'arme. Sans clé ou sans motion trouvée, rien n'est
  installé.
- `remove()`, appelé dans le destructeur avant la suppression du modèle.
- `trigger(event)` : pour chaque calque abonné à cet événement :

  ```cpp
  CBlend* B = ka->LL_PlayCycle(part, mid, TRUE /*mixin*/, accrue, falloff,
                               speed, TRUE /*noloop*/, nullptr, nullptr, channel);
  ```

### 6.2 Les déclencheurs (tous existent déjà dans le code)

| Déclencheur | Où s'accrocher |
|---|---|
| `shot` | `CWeaponMagazined::OnShot()` |
| `fire_end` | `CWeaponMagazined::FireEnd()` / `StopShooting()` |
| `mark:<nom>` | `CHudItem::OnMotionMark()` : les marques d'events dans les anims HUD y passent déjà |
| état | `OnStateSwitch` (reload, draw, holster...) |
| Lua | une fonction exportée `hud_layer_play(nom)` pour les moddeurs |

Les marques d'events sont l'outil naturel de l'animateur : il pose une marque `handle_drop` dans
l'anim de reload, et le calque du carry handle démarre exactement à cette frame.

### 6.3 Gestion de la relance (obligatoire)

En tir automatique, le déclencheur `shot` revient 10 à 15 fois par seconde. Sur les canaux 2 et 3, le
moteur **ne coupe pas** l'ancienne instance quand on en lance une nouvelle (c'est justement ce qui les
rend indépendants). Il faut donc gérer ça soi-même :

- **crossfade** (recommandé) : on passe l'ancienne instance en fondu de sortie
  (`B->set_falloff_state(); B->blendFalloff = X;`), puis on lance la nouvelle. Dans un même canal,
  deux instances s'**interpolent** selon leurs poids (elles ne s'additionnent pas), ce qui donne une
  transition douce de l'ancienne vers la nouvelle.
- **restart** : on remet l'instance existante à zéro (`timeCurrent = 0`). C'est simple, mais ça
  provoque un saut visible si l'anim était au milieu d'une oscillation.
- **ignore** : on ne relance pas tant que l'instance tourne.

Limites à respecter (sinon bug silencieux ou plantage) :

- **16 blends maximum par bone** (`MAX_BLENDED`). Si le bone est plein, une nouvelle anim de calque
  est **ignorée sans message**.
- **64 blends maximum par partition et par modèle.** Dépasser ce nombre fait déborder le tableau.
- Il faut donc garder un **petit nombre d'instances vivantes par calque** (3 ou 4 au plus), en
  mettant les plus anciennes en fondu.

Attention aux pointeurs `CBlend*` gardés d'une frame à l'autre : ils viennent d'un pool et peuvent être
réutilisés après destruction. Avant d'y toucher, vérifier `blend_state() != eFREE_SLOT`, et que
`motionID` et `channel` correspondent toujours. C'est le même garde-fou que dans
`character_hit_animations.cpp`.

### 6.4 Le poids (« power ») d'un calque

Point vérifié dans le code et peu intuitif : **à l'intérieur d'un canal, une instance seule est
appliquée à 100 %, quel que soit son poids** (`MixInterlerp`, cas `b_count == 1`). Le poids d'une
instance ne sert qu'à la répartir face aux autres instances du même canal.

Pour régler l'intensité d'un calque, il faut utiliser le **facteur de canal** :
`ka->LL_SetChannelFactor(channel, power)`. Ce facteur est propre à chaque modèle et **partagé par tout
le canal**. D'où la règle simple : **un type de calque par canal** (par exemple canal 2 pour le
ballottement, canal 3 pour l'avance de bande). Pour obtenir un vrai fondu d'entrée et de sortie
d'intensité, le contrôleur peut faire varier ce facteur à chaque frame.

### 6.5 Points mineurs

- Quand l'arme HUD est recréée (changement d'arme, d'accessoire...), les calques en cours
  disparaissent. C'est acceptable.
- Le canal 1 (interpolation, pas additif) pourrait servir de calque qui **remplace** la pose de
  certains bones, par exemple un carry handle entièrement piloté par son propre anim. Comme une
  instance seule y est aussi appliquée à 100 %, le fondu d'entrée et de sortie ne marche qu'à travers
  le facteur de canal. À tester avant de s'en servir.

---

## 7. Récapitulatif des pièges pour l'animateur

| Symptôme en jeu | Cause probable |
|---|---|
| La pièce saute à la fin du calque | la dernière frame n'est pas égale à la frame 0 |
| Toute l'arme est décalée pendant le calque | un bone non concerné bouge dans l'anim du calque |
| La pièce part en vrille | rotation additive trop grande, ou frame 0 très éloignée de la pose de l'idle |
| La bande « recule » à chaque tir | le cycle d'avance ne va pas exactement d'un maillon au suivant |
| La bande avance trop vite ou trop lentement | durée de l'anim ou formule de vitesse (§5.1) |
| Rien ne se passe en tir auto | relance non gérée : bone saturé (§6.3) |

---

## 8. Alternative ou complément : le procédural

Un calque animé, c'est un mouvement **joué** : il est identique à chaque fois. Si en tir soutenu ça
fait trop « répété », on peut ajouter une couche **procédurale** :

- chaque balle est un ressort amorti qui suit la balle précédente (une chaîne) ;
- une impulsion est donnée à chaque tir ;
- l'accélération réelle de l'arme (recul, mouvements de souris, marche) sert de moteur.

Sur le coût : une trentaine de bones avec quelques sous-pas par frame représentent quelques
microsecondes. Le vrai coût, c'est le **réglage**, pas le processeur.

Pour que le retour du cycle d'avance (§5.1) ne crée pas de fausse secousse, le ressort doit travailler
sur des **décalages par rapport à la pose animée**, pas sur des positions absolues.

Les deux approches se combinent. Dans X-Ray, le callback d'un bone est appelé **après** le mélange
des canaux (`CKinematics::CLBone`), donc le procédural s'ajoute par-dessus l'animé sans conflit.

**Recommandation : commencer par les calques animés.** C'est moins de code, l'animateur garde le
contrôle, et le mélange utilisé est déjà éprouvé en jeu (réactions des PNJ aux impacts). On ajoute le
procédural seulement si le résultat manque de vie en tir prolongé.

---

## 9. Références dans le code source (build `xray-monolith`)

| Fichier | Ce qu'on y trouve |
|---|---|
| `src/Layers/xrRender/Animation.cpp` | règles de mélange des 4 canaux |
| `src/Layers/xrRender/SkeletonAnimated.cpp` | `LL_PlayCycle` (fondu par canal), `IBlendSetup` (`fall_at_end`), `LL_BuldBoneMatrixDequatize` (soustraction de la frame 0), `LL_BoneMatrixBuild` |
| `src/Layers/xrRender/AnimationKeyCalculate.h` | `MixInterlerp`, `MixChannels`, `key_add`, `key_sub` |
| `src/Layers/xrRender/KinematicAnimatedDefs.h` | `MAX_BLENDED = 16`, `MAX_CHANNELS = 4` |
| `src/Layers/xrRender/SkeletonRigid.cpp` | `Bone_Calculate` (tous les canaux), `CLBone` (callback après le mélange) |
| `src/Include/xrRender/KinematicsAnimated.h` | interface disponible depuis `xrGame` |
| `src/xrGame/player_hud.cpp` | `attachable_hud_item::anim_play` (canal 0, toutes partitions) |
| `src/xrGame/character_hit_animations.cpp` | exemple existant d'anims additives sur les canaux 2 et 3 |
| `src/xrGame/HudItem.cpp` | `OnMotionMark` (marques d'events des anims HUD) |
| `src/xrGame/WeaponMagazined.cpp` | `OnShot`, `FireEnd`, `HUD_VisualBulletUpdate` |

---

## 10. Limites techniques à connaître (vérifiées dans le code du build `xray-monolith-bodycam`, 08/10/2026)

Section ajoutée après relecture du code. Elle complète §4 (guide animateur) avec les limites sur **quels bones** et **quel type de mouvement**
un calque peut piloter.

### 10.1 Les trois limites sur les bones
1. **Le bone doit exister dans le squelette de l'arme et avoir des clés dans la motion du calque.**
   Un calque ne crée rien : il ne fait que lire les pistes de la motion. Un bone absent de l'armature exportée, ou présent mais sans
   clé dans cette motion, n'est pas touché. (La motion doit donc être exportée avec les bones voulus, dans l'OMF de l'arme.)
2. **Rotation et translation seulement. L'échelle (scale) des bones n'est pas animée.**
   Chaque bone n'a, par image, qu'une rotation (quaternion) et une position ; il n'existe pas de piste d'échelle dans le format de
   motion du moteur. Un effet « gonfler / rétrécir » ne passera pas par un calque. (Pour faire apparaître / disparaître une pièce,
   c'est la visibilité du bone, gérée à part, comme les balles de chargeur.)
3. **Pas le bone racine.** À chaque animation d'arme, le moteur remet le bone racine de l'arme à l'identité
   (`player_hud.cpp`, `anim_play` : `set_callback_overwrite(TRUE)` puis `mTransform.identity()`), parce que la position de l'arme dans
   la vue est calculée ailleurs (HUD, bodycam). Une animation de calque sur ce bone serait écrasée. Tous les autres bones (enfants du
   racine) sont utilisables.

### 10.2 Autres points à connaître côté animateur
- **Rig, IK et contraintes : tout doit être « bakée ».** Le moteur ne lit que des clés (rotation + position par bone et par image).
  Les chaînes IK, les contraintes à gradient, les drivers, les simulations physiques de Blender sont des outils de **fabrication** :
  on les utilise pour produire le mouvement, puis on le bake dans les bones à l'export. Le jeu ne les évalue pas. Toute souplesse
  disponible dans Blender est donc utilisable, tant que le résultat final est bakable en clés sur les bones de l'armature exportée.
  Prévoir un échantillonnage à chaque image (30 fps) pour ne pas perdre le mouvement.
- **Le calque s'additionne à l'animation principale** : `pose finale = pose du canal 0 ⊕ [calque(t) ⊖ calque(frame 0)] × facteur`.
  La différence est calculée comme une rotation appliquée **avant** la rotation du bone (repère du parent) : sur de petits débattements
  c'est transparent, sur de grands angles l'ordre des rotations peut décaler un peu le résultat (à tester en jeu).
- **La dernière frame doit être égale à la frame 0.** Après la dernière image, le calque est maintenu environ 0,5 s en fondu de sortie,
  mais une animation seule dans son canal est appliquée à 100 % tant qu'elle existe : le fondu ne l'atténue pas visuellement.
  Si la dernière frame n'est pas au repos, la pièce saute au moment où le calque disparaît.
- **16 animations simultanées maximum par bone** (toutes confondues : animation principale, anciennes animations en fondu, calques).
  En pratique on en utilise 3 à 5. Au-delà, un calque sans boucle est ignoré sans message (ne pas boucler un calque : une animation
  bouclée sur un bone plein peut supprimer une animation du canal 0).
- **Une seule sorte de calque par canal** : l'intensité (facteur) est partagée par tout le canal. Plusieurs instances du même calque
  dans un canal s'interpolent (elles ne s'additionnent pas) ; des calques sur deux canaux différents (2 et 3) s'additionnent bien.
- **Le calque ne touche pas les mains**, seulement le modèle de l'arme (déjà dit en §4.3).
- **Ne pas animer les mêmes bones avec le jiggle procédural** (`jiggle_bones`, doc 07). Le jiggle s'exécute **après** le mélange des
  animations (callbacks de bones, `CKinematics::CLBone`), donc il se cumule proprement par-dessus les calques ; mais deux systèmes
  qui pilotent le même bone se contredisent. Un bone = un seul système.
- **Implémentation :** voir la section 11 (contrôleur C++ écrit le 08/10/2026, syntaxe de configuration définitive dans §11.3).


---

## 11. Implémentation et mode d'emploi (08/10/2026)

**Statut : écrit, compilé (0 erreur), déployé, pas encore testé en jeu.** Une arme sans clé `hud_layers` se comporte exactement comme avant
(aucune animation lancée, aucun calcul par frame ; seuls un test de présence de clé au chargement de l'arme et quelques tests « y a-t-il
des calques ? » aux événements, négligeables).

### 11.1 Ce que fait le code (explication)

Fichiers :

| Fichier | Rôle |
|---|---|
| `xrGame/hud_anim_layers.h/.cpp` (nouveaux) | le contrôleur `CHudAnimLayers` : lecture de la config, déclencheurs, gestion des instances |
| `xrGame/player_hud.h/.cpp` | un contrôleur par arme HUD (`attachable_hud_item::m_layers`), installé à la fin de `load()`, nettoyé dans le destructeur ; déclencheur `anim:` dans `anim_play()` |
| `xrGame/WeaponMagazined.cpp/.h` | déclencheurs `shot` et `burst_start` (dans `OnShot`) |
| `xrGame/WeaponFire.cpp`, `Weapon.h` | déclencheur `fire_end` (dans `FireEnd`) |
| `xrGame/HudItem.cpp` | déclencheur `mark:` (dans `OnMotionMark`, avant l'appel Lua existant qui reste intact) |
| `xrGame/script_sound_script.cpp` | fonction Lua `hud_layers.play("<section du calque>")` |
| `xrGame/console_commands.cpp` | commande console `g_hudlayers_debug` (0/1) |

Déroulement :
1. **Chargement de l'arme HUD** : `install()` lit `hud_layers` dans la section HUD de l'arme (celle qui contient `item_visual`). Pour chaque calque
   il résout les noms de motions avec `ID_Cycle_Safe` sur le modèle de l'arme (une motion introuvable = avertissement dans le log, calque ignoré), lit
   les réglages, et règle le facteur du canal (`LL_SetChannelFactor`). Sans clé `hud_layers`, il ne fait rien.
2. **Un événement arrive** (tir, fin de rafale, nom d'animation HUD, marque, appel Lua). Le contrôleur parcourt ses calques (8 au plus) et déclenche
   ceux qui y sont abonnés.
3. **Déclenchement d'un calque** (`fire()`), dans l'ordre :
   - il oublie les instances disparues (le moteur recycle les emplacements de blend : on vérifie que le blend existe encore, avec la même motion et le même canal) ;
   - selon `retrigger` : `ignore` s'arrête là si une instance joue encore ; `restart` ramène l'instance en cours à zéro ; `crossfade` met les instances en
     cours en fondu de sortie (durée `blend_out`) ;
   - il garde au plus `max_instances` instances vivantes (les plus anciennes sont éliminées vite) ;
   - il choisit une motion parmi les variantes (jamais la même deux fois de suite s'il y en a plusieurs) ;
   - il lance la motion avec `LL_PlayCycle(..., noloop = TRUE, canal 2 ou 3)` sur chaque partition de l'arme (ou seulement celle demandée par `part`). Le
     **moteur force l'arrêt à la fin** (pas de boucle) quoi que dise l'OMF : le réglage « stop at end » de la motion n'est pas nécessaire ;
   - `speed_rpm` : la vitesse est calculée pour qu'un passage dure exactement un intervalle entre deux tirs de l'arme (RPM réel de l'arme, mises à niveau comprises) ;
   - `start_min` / `start_max` : décalage de départ aléatoire (en part de la durée du calque).
4. **Fin d'un calque** : entièrement gérée par le moteur (voir §2.2 : fondu de fin automatique puis destruction). Le contrôleur n'a aucun travail par frame.

### 11.2 Ce qui est garanti pour les autres armes
- Aucune arme n'est modifiée tant qu'elle ne déclare pas `hud_layers`.
- Les armes des PNJ n'ont pas de modèle HUD : jamais concernées (gardes `ParentIsActor()` et `IsAttachedToHUD()`).
- Les animations principales (canal 0), les mains, le jiggle, les sons et le recul ne sont pas touchés.
- Seuls changements visibles pour tout le monde : une commande console de plus (`g_hudlayers_debug`) et la fonction Lua `hud_layers.play`.

### 11.3 Configuration (codification)

Syntaxe identique à celle du jiggle (`![section]` = surcharge DLTX de la section **HUD** de l'arme, pas de la section d'objet).

```ini
![wpn_pkm_hud]
hud_layers = wpn_pkm_layer_belt_feed, wpn_pkm_layer_belt_settle

; --- calque court, relancé à chaque balle, durée = un intervalle entre deux tirs ---
[wpn_pkm_layer_belt_feed]
motion      = belt_feed_cycle
channel     = 3
trigger     = shot
retrigger   = restart
speed_rpm   = true

; --- calque long : impulsion à chaque tir, puis oscillation qui s'éteint après le dernier tir ---
[wpn_pkm_layer_belt_settle]
motion      = belt_settle_a, belt_settle_b, belt_settle_c    ; variantes tirées au hasard
channel     = 2
trigger     = shot, anim:anm_shots*
retrigger   = crossfade
blend_in    = 0.03
blend_out   = 0.10
start_min   = 0.0
start_max   = 0.0
```

Clé de la section HUD :

| Clé | Valeur | Rôle |
|---|---|---|
| `hud_layers` | noms de sections séparés par des virgules (8 au plus) | liste des calques de cette arme. Absente = aucun calque |

Clés d'une section de calque :

| Clé | Défaut | Rôle |
|---|---|---|
| `motion` | (obligatoire) | nom d'une ou plusieurs motions de l'OMF de l'arme ; plusieurs = variantes (tirage au hasard, jamais deux fois la même à la suite) |
| `channel` | `3` | canal additif : `2` ou `3` (autre valeur = 3 avec un avertissement). **Un seul type de calque par canal** (voir vigilance) |
| `trigger` | (aucun) | un ou plusieurs déclencheurs séparés par des virgules (tableau ci-dessous). Sans déclencheur, le calque ne part que par `hud_layers.play` |
| `retrigger` | `crossfade` | `crossfade`, `restart` ou `ignore` (tableau ci-dessous) |
| `speed` | `1.0` | multiplicateur de vitesse |
| `speed_rpm` | `false` | `true` = la vitesse est calée pour qu'un passage dure exactement un intervalle entre deux tirs (RPM réel de l'arme) |
| `blend_in` | `0.03` | durée (s) d'entrée en fondu de la nouvelle instance |
| `blend_out` | `0.10` | durée (s) de sortie en fondu des instances remplacées (`crossfade`) |
| `start_min`, `start_max` | `0` | décalage de départ aléatoire en part de la durée (0 à 0.99) : utile pour casser la répétition |
| `part` | `-1` | numéro de partition de bones à animer (`-1` = toutes) ; une seule partition coûte un peu moins |
| `max_instances` | `3` | instances vivantes au plus (1 à 6) |
| `power` | `1.0` | facteur du canal (intensité du calque) ; il s'applique à **tout le canal** de ce modèle |

Déclencheurs (`trigger`) :

| Valeur | Quand |
|---|---|
| `shot` | à chaque balle tirée par l'arme du joueur (hors mode lance-grenade) |
| `burst_start` | au premier tir d'une rafale (chaque pression de gâchette en semi-auto) |
| `fire_end` | à la fin de la rafale (relâchement, chargeur vide, rechargement, rangement) ; seulement après une vraie rafale |
| `anim:<nom>` | au démarrage d'une animation HUD dont le nom (alias `anm_...`) correspond ; `*` remplace n'importe quelle suite (`anim:anm_shots*`) ; le nom comparé est le nom final, après les scripts Lua qui réécrivent les variantes |
| `mark:<nom>` | quand l'animation HUD en cours franchit la marque d'événement de ce nom (posée dans l'animation des mains par l'animateur) |

Modes de relance (`retrigger`) quand l'événement revient pendant que le calque joue encore :

| Mode | Effet | Pour quoi faire |
|---|---|---|
| `crossfade` | nouvelle instance à la frame 0, l'ancienne s'efface | calque long qui accompagne la rafale (impulsion pendant, oscillation après) ; calque court déclenché à chaque tir |
| `restart` | l'instance en cours revient à la frame 0 | calque très court calé sur le tir (`speed_rpm`) |
| `ignore` | rien ne se passe tant qu'il joue | calque long à ne jouer qu'une fois (par exemple au début de la rafale avec `burst_start`) |

Lua : `hud_layers.play("wpn_pkm_layer_belt_settle")` lance un calque de l'arme tenue en main principale (renvoie `true` s'il existe).

Console : `g_hudlayers_debug 1` écrit dans le log les calques installés et chaque déclenchement (variante, nombre d'instances). Les erreurs de config
(`! [hud_layers] ...`) sont toujours écrites. Après modification d'un `.ltx`, ranger puis ressortir l'arme suffit (pas de redémarrage).

### 11.4 Mini tutoriel pour l'animateur

1. **Dans Blender**, dans l'armature de l'arme, anime **uniquement les bones du calque** (balles de la bande, carry handle, levier). Les IK, contraintes, gradients et
   simulations servent à *fabriquer* le mouvement ; à la fin, **bake** le résultat en clés sur ces bones (une clé par image à 30 fps).
2. **Frame 0 = pose de repos** des bones du calque (même pose que dans l'idle) : le moteur retire la frame 0, tout ce qui y est ne compte pas.
3. **Dernière frame = frame 0** (retour exact au repos), sinon la pièce saute quand le calque disparaît.
4. **Les bones qui ne servent pas restent parfaitement immobiles** dans cette motion.
5. **Nomme la motion** de façon claire (`belt_settle`, `handle_drop`) et exporte-la **dans l'OMF de l'arme** (celui de `item_visual`, pas celui des mains), comme une
   motion normale. Pas de boucle à régler : le moteur l'impose.
6. **Pour un calque lancé à chaque tir (`speed_rpm`)** : une avance de bande d'un maillon (chaque balle va à la position de la suivante), la dernière frame
   équivalant visuellement à la première. Pour une bande, aucun réindexage n'est nécessaire.
7. **Pour casser la répétition en tir soutenu**, fais plusieurs variantes de la **première partie** du calque (les premières frames sont celles qui reviennent à
   chaque tir) et liste-les dans `motion = a, b, c`.
8. **Donne au programmeur / à la config** : le nom des motions, les bones touchés, ce qui doit déclencher (tir, fin de rafale, une animation précise, une marque)
   et si le calque est court (calé sur le tir) ou long.
9. **Test sans animation spéciale** : mets comme `motion` le nom de la motion de tir de l'arme elle-même (celle que `anm_shots` désigne), canal 3, `trigger = shot`.
   Comme l'additif ajoute la différence à la pose du tir, le recul de l'arme devrait paraître exagéré à chaque balle : cela prouve que la chaîne complète marche.

### 11.5 Points de vigilance

- **Une seule sorte de calque par canal** : le facteur `power` est partagé par tout le canal. Si deux calques d'un même canal donnent des `power` différents, c'est
  le dernier lu qui reste.
- **Une motion sans piste pour un bone ne le touche pas** : une motion exportée sans les bones voulus ne fera rien de visible (vérifier l'export avant d'accuser le moteur).
- **16 animations par bone au maximum** (toutes confondues). Avec `max_instances` à 3, un calque en consomme 3 au plus par partition. Au-delà de la limite, le
  moteur ignore le nouveau calque sans message.
- **`anim:` compare le nom d'alias** (`anm_shots...`), pas le nom de la motion dans l'OMF. Si un script Lua change le nom avant le jeu de l'animation, c'est le
  nom final qui compte.
- **`mark:`** ne marche que si la marque existe dans l'animation HUD en cours et que cette animation est jouée comme animation « stop at end » du système HUD.
- **`shot` ne couvre pas le lance-grenade** (son `OnShot` est séparé) ; à ajouter plus tard si besoin.
- **`fire_end` n'est lancé qu'après une rafale réelle** (au moins un tir), pas à chaque `FireEnd`.
- **Réglage de `speed_rpm`** : si le calque est trop rapide ou trop lent, vérifier que sa durée correspond à un intervalle de tir et non à plusieurs.
- **Un bone = un seul système** : ne pas mettre les mêmes bones dans un calque et dans le jiggle procédural (`jiggle_bones`).
- **Jamais le bone racine, jamais l'échelle** (voir §10).
- **Les mains ne bougent pas avec le calque** : si une main tient la pièce animée, ne pas déclencher pendant l'animation correspondante, ou garder l'amplitude faible.
- **Recréer l'arme HUD** (changement d'arme, d'accessoire, rechargement du HUD) arrête les calques en cours : acceptable.

### 11.6 Diagnostic

| Symptôme | Cause probable / action |
|---|---|
| Aucun effet | `g_hudlayers_debug 1` : le calque est-il listé à l'installation ? sinon la clé `hud_layers` n'est pas dans la **section HUD** ; l'événement arrive-t-il (une ligne `fired` à chaque déclenchement) ? |
| `! [hud_layers] ... motion '...' not found` | le nom de motion n'est pas dans l'OMF de l'arme (faute de frappe, ou motion exportée dans un autre OMF) |
| Le calque se déclenche mais rien ne bouge | les bones concernés ne sont pas animés dans la motion, ou le calque est étouffé par un bone saturé (16 animations) |
| La pièce saute à la fin | dernière frame différente de la frame 0 |
| Toute l'arme bouge | un bone non concerné est animé dans la motion du calque |
| Le mouvement ne montre que le début du calque | normal en tir soutenu avec `crossfade` / `restart` ; utiliser `ignore` ou `burst_start` pour un calque long |

### 11.7 À vérifier en jeu (checklist)
1. Une arme sans `hud_layers` : aucun changement (comparer avec l'exe précédent).
2. Test §11.4-9 (motion de tir de l'arme elle-même en calque) : le recul est exagéré, le log affiche les déclenchements.
3. `retrigger` : trois essais (`crossfade`, `restart`, `ignore`) en tir automatique.
4. `anim:anm_shots*` et `anim:anm_shots_aim*` : le calque part en hanche **et** en visée.
5. Fin de rafale : le calque long finit son mouvement après le dernier tir.
6. Changement d'arme pendant un calque : pas de plantage, pas de bone figé.
7. `speed_rpm` avec une arme dont le RPM est modifié par une mise à niveau.


---

## 12. Export depuis Blender avec le plugin « io_scene_xray » (vérifié dans le code du plugin, 08/10/2026)

Plugin lu : `Blender 3.6 / scripts / addons / io_scene_xray` (export OMF : `formats/omf/exp.py`, réglages d'action : `props/action.py`, `panels/action.py`).
Les noms des boutons peuvent varier légèrement selon la version ; la logique ci-dessous vient du code.

### 12.1 Comment le plugin exporte une animation (ce qui compte pour l'animateur)
- **Une motion = une Action Blender**, à condition qu'elle soit dans la liste **« Motions » de l'armature** (panneau X-Ray de l'objet armature). Une action
  absente de cette liste n'est pas exportée. Le nom de la motion est le nom de l'action (ou un « export name » si on l'active).
- **L'export échantillonne la pose à chaque image entière** de la plage de l'action (début à fin) : la durée de la motion = nombre d'images, et le plugin
  lit la pose évaluée de **chaque bone** (rotation + position). Conséquences :
  - les **IK, contraintes et drivers sont évalués au moment de l'échantillonnage** : le bake se fait donc tout seul à l'export ;
  - **le nombre de keyframes dans Blender n'a aucune importance** (« davantage de keyframes » ne sert à rien : le moteur reçoit une valeur par image) ;
  - le moteur joue les motions à **30 images par seconde** : animer et exporter avec une scène à **30 fps**, sinon la vitesse change.
- **Qualité** : par défaut la translation est stockée sur 8 bits par axe (256 niveaux entre le minimum et le maximum du bone dans la motion). Pour des
  mouvements fins (jiggle, bande de balles), cocher **« High Quality »** à l'export (16 bits).
- **Réglages de l'action** (panneau X-Ray de l'action) : FPS, Speed, Accrue, Falloff, Bone Part, indicateurs (Type FX, Stop, No Mix, Sync, ...).

### 12.2 Réponse : comment faire pour un calque
Un calque est **une motion à part**, pas un réglage de `anm_shots`. Procédure :
1. **Garde `anm_shots` comme aujourd'hui** (pour la bande : statique, ou comme tu veux). Cette animation n'est pas modifiée.
2. **Crée une nouvelle Action** pour le calque, par exemple `belt_jiggles_layer` (tu peux dupliquer `anm_shots` pour repartir de la même pose, puis
   remplacer l'animation des bones de la bande). Le nom est libre ; **le préfixe `anm_` n'est pas nécessaire** (il ne sert qu'aux alias des animations HUD,
   les calques désignent directement le nom de la motion dans la config : `motion = belt_jiggles_layer`).
3. **Dans cette action** :
   - bones de la bande animés, tous les autres **exactement immobiles et à la pose de repos** sur toutes les images ;
   - **image de départ = pose de repos** ; **dernière image = pose de repos** (même pose que la première) ;
   - durée libre (par exemple 24 images = 0,8 s).
4. **Réglages de l'action** : FPS 30, Speed 1 ; **« Type FX » désactivé (obligatoire)** : une motion FX n'est pas une motion « cycle », le moteur ne la trouverait
   pas ; « Stop at end » sans importance (le moteur impose l'arrêt à la fin), « Bone Part » laissé vide ; Accrue et Falloff ne servent pas pour un calque
   (c'est `blend_in` / `blend_out` de la config qui comptent).
5. **Ajoute l'action à la liste « Motions » de l'armature**.
6. **Exporte dans l'OMF de l'arme** (celui de `item_visual`) en mode **ADD** (voir 12.3) avec « High Quality » coché.
7. Écris le calque dans la configuration (§11.3) avec `motion = belt_jiggles_layer`.

Une motion de calque n'a **pas besoin d'équivalent dans l'OMF des mains** : les calques ne passent pas par les alias `anm_` des mains.

### 12.3 Modes d'export de l'OMF (plugin)
| Mode | Effet | Quand l'utiliser |
|---|---|---|
| **ADD** | ajoute au fichier OMF existant uniquement les motions qui n'y sont pas, **sans toucher aux autres** (leurs réglages et leurs marques sont conservés) | ajouter un calque à l'OMF d'une arme |
| **REPLACE** | remplace les motions choisies (et/ou les bone parts) en gardant le reste | corriger un calque déjà exporté |
| **OVERWRITE** | réécrit tout le fichier à partir de la scène Blender | à éviter pour un OMF existant (toutes ses motions devraient être dans la scène ; les bones sans groupe provoquent une erreur) |

En ADD et REPLACE, le plugin lit le fichier OMF existant et reprend l'ordre de ses bones : l'armature de la scène doit correspondre au squelette de l'arme.

### 12.4 Limites du plugin à connaître
- **Marques d'événements (motion marks) : non gérées.** À l'export, le plugin écrit 0 marque pour une nouvelle motion ; à l'import il les ignore (avertissement). Les motions
  **déjà présentes** dans l'OMF gardent leurs marques en ADD / REPLACE. Pour le déclencheur `mark:<nom>` (§11.3), les marques se posent dans l'OMF **des mains**
  (la marque est lue sur l'animation des mains) avec un autre outil, pas avec ce plugin :
  - **OMF Editor** (version de ValeroK, `D:/STALKER ANO/tools/OMF_Editor.exe`) : en inspectant le programme, on y trouve un groupe **« Motion Marks »** (bouton
    d'ajout, nom de la marque, temps de début et de fin) à côté des champs Speed / Power / Accrue / Falloff et des cases Stop At End / No Mix. C'est donc l'outil
    à utiliser pour poser les marques sur la motion des mains. *(Constaté dans le fichier exécutable, pas essayé.)*
  - Une marque est un **intervalle de temps** (début, fin) en secondes dans la motion, avec un nom. Le moteur déclenche `mark:<nom>` au moment où le temps de
    l'animation **entre** dans l'intervalle (image précédente hors intervalle, image courante dedans). Donc **donner à l'intervalle au moins ~0,1 s de large** :
    un intervalle plus étroit qu'une image de jeu peut être sauté, surtout à bas FPS.
  - Le temps de la marque est comparé à la durée réellement écoulée depuis le début de l'animation : si l'animation est jouée à une vitesse différente de 1
    (`anm_speed`), la marque se décale d'autant (à vérifier en jeu).
- **Pas d'échelle de bone** (le format n'en a pas, cf. §10).
- Un bone **non exportable** (case « exportable » décochée) n'est pas dans la motion.

### 12.5 Pièges d'export
| Symptôme en jeu | Cause probable |
|---|---|
| Le calque ne se déclenche pas, `motion '...' not found` dans le log | action absente de la liste Motions de l'armature, ou exportée dans le mauvais OMF, ou « Type FX » coché |
| L'animation est trop rapide / trop lente | scène Blender pas à 30 fps |
| Mouvement « en escalier » sur les petits déplacements | « High Quality » non coché (translation 8 bits) |
| Toute l'arme bouge | un bone hors calque n'est pas à la même pose sur toutes les images (parent animé, contrainte, dérive) |
| La pièce saute à la fin | dernière image différente de la première |
