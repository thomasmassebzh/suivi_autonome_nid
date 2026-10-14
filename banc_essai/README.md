# Banc d'essai — validation pas à pas sur la carte

Programme de test de l'**ESP32-P4X-Function-EV-Board**, distinct du firmware de terrain. Il compile
directement les modules du firmware (`../firmware/components/nid_board`, `nid_capture`, `nid_storage`,
`nid_config`, `nid_detect`, `nid_ir`, `nid_power`, `nid_clock`, sans les copier) pour valider sur la
carte, une fonction après l'autre, **le vrai chemin du firmware** : caméra → incrustation date/heure →
H.264 + son AAC → MP4 sur la carte SD (fichier `.tmp` puis nom définitif, `_INCOMPLET` après une
coupure), détection PIR avec la logique du firmware, photos, nuit, son, batterie, carte SD.

L'écran 7 pouces sert de tableau de bord (onglets, pastilles d'état, boutons tactiles). Le guide
d'essais complet (exemples Espressif puis banc) est dans [docs/essais_sur_carte.md](../docs/essais_sur_carte.md).

Le banc n'utilise ni veille profonde ni Wi-Fi : il reste éveillé et toute erreur est affichée sans
redémarrer la carte.

## Matériel

- ESP32-P4X-Function-EV-Board (puce rév. ≥ 3.1), caméra OV5647 (ou SC2336 du kit) sur le connecteur
  MIPI-CSI avec la nappe « directe » (bandes du même côté aux deux bouts) ;
- carte microSD formatée en **FAT32, une seule partition** (le banc ne formate jamais une carte) ;
- PIR HC-SR501 : OUT → **GPIO4**, VCC → 5 V, GND ; résistance de 100 kΩ entre OUT et GND conseillée ;
- écran 7 pouces du kit (1024×600, EK79007, dalle tactile GT911), facultatif mais conseillé :
  - nappe « inversée » (bandes de côtés opposés) entre le connecteur MIPI-DSI de la carte et J3 de
    l'adaptateur de l'écran ;
  - fil **GPIO27** (J1 broche 38) → **RST_LCD** (J6 de l'adaptateur) ;
  - fil **GPIO26** (J1 broche 31) → **PWM** (J6), rétroéclairage ;
  - dalle tactile : nappe tactile de l'écran sur l'adaptateur ; SDA/SCL de l'adaptateur sur le bus I2C
    de la carte (GPIO7/GPIO8), comme pour le BSP Espressif ;
  - alimentation de l'écran : câble USB sur J1 de l'adaptateur (conseillé), ou 5V/GND depuis J1 de la carte.
- facultatif : LED IR sur GPIO32 (MOSFET), filtre IR-cut, pont diviseur de batterie sur GPIO21 (menu
  « Suivi nid — Carte et brochage »). Non câblés, ils sont signalés « non câblé » sans gêner le reste.

## Compiler, flasher, moniteur

Installation de l'environnement (VS Code, extension ESP-IDF, ESP-IDF v6.1) : voir le
[README racine](../README.md), sections 1 à 6. Pour le banc :

1. `Fichier` › `Ouvrir le dossier…` › **`banc_essai/`** (et non `firmware/` ni la racine du dépôt) :
   c'est un projet ESP-IDF à part entière, avec ses propres `sdkconfig.defaults` et `partitions.csv`.
2. **`ESP-IDF: Set Espressif Device Target`** › **`esp32p4`**.
3. Facultatif : **`ESP-IDF: SDK Configuration Editor`** › menu **« Banc d'essai »** (voir Réglages).
4. **`ESP-IDF: Build your Project`** (la première compilation télécharge les composants : connexion
   Internet nécessaire). Après la première compilation réussie, versionner `banc_essai/dependencies.lock`.
5. Brancher le port **USB Serial/JTAG** de la carte, **`ESP-IDF: Select Port to Use`**, méthode de flash
   **UART**, puis **`ESP-IDF: Build, Flash and Start a Monitor on Your Device`**.

Ligne de commande (terminal ESP-IDF) : `cd banc_essai && idf.py set-target esp32p4 && idf.py build flash monitor`.

Après une modification de `sdkconfig.defaults` (c'est le cas avec cette version : `CONFIG_VFS_MAX_COUNT=16`) :
**`ESP-IDF: Full Clean Project`**, supprimer `banc_essai/sdkconfig`, recompiler.

## Choisir les fonctions à tester

Pour tester une partie sans que le matériel absent ne provoque d'erreurs en cascade, chaque fonction
peut être retirée ou désactivée : écran, tactile, caméra (vidéo, photo), son, carte SD, PIR et
détection, nuit (LED IR, IR-cut, monochrome), batterie, température, photo de suivi, console.

1. **À la compilation** : menu **« Banc d'essai › Fonctions activées »**, une option par fonction. Une
   fonction retirée n'est ni compilée dans la liste, ni initialisée, ni appelée.
2. **Au démarrage** : écran de sélection (tout début de `app_main`), la liste des fonctions compilées
   cochées ou non :
   - écran tactile : toucher une ligne la bascule, toucher **DÉMARRER** lance le banc ;
   - bouton BOOT : **appui court** = ligne suivante, **appui long** (1 s) = basculer la ligne, ou
     démarrer si le curseur est sur DÉMARRER ;
   - console : `fonctions` (liste), `fonctions on|off <nom>`, `fonctions go` (démarrer) ;
   - sans action, le banc démarre au bout de **5 s** (menu « Écran de sélection… », 0 = pas d'écran
     de sélection) sur la dernière sélection ; chaque action repousse le démarrage à 10 s.
   La sélection est **mémorisée en NVS** pour les démarrages suivants. Noms pour la console : `ecran`,
   `tactile`, `camera`, `son`, `sd`, `pir`, `nuit`, `batterie`, `temperature`, `suivi`, `console`.
3. **À l'initialisation** : chaque fonction choisie est initialisée ; un échec la marque **« en
   défaut »** (journal série, ligne « Fonctions : … » du bandeau) sans bloquer les autres. Une carte SD
   ou une caméra en défaut est réessayée à la prochaine vidéo ou photo (et rétablie si l'essai réussit).

Dépendances : le son exige la caméra ; la photo de suivi, la caméra et la carte SD ; le tactile,
l'écran (une fonction dont la dépendance manque est « désactivée »). Partout à l'écran, une fonction
désactivée ou en défaut s'affiche « désactivé » ou « en défaut » ; les commandes correspondantes
répondent de même.

L'écran, le tactile et la console servent à la sélection : ils sont démarrés avant elle s'ils étaient
choisis au démarrage précédent. Désactiver l'écran l'éteint après la sélection ; la console, une fois
lancée, reste active jusqu'au redémarrage (`redemarrer`). Réactivés pendant la sélection, ils sont
démarrés juste après. Après le démarrage, `fonctions on|off <nom>` vaut pour le démarrage suivant.

## Écran

```
+------------------------------------------------------------------------------+
| DÉTECTION |  PHOTO  |  NUIT  |  SON  |  SYSTÈME                  (onglets)   |
+-----------------------------------------+------------------------------------+
|                                         | PIR (GPIO4)                        |
|                                         |            ACTIF / REPOS           |
|      vue caméra en direct               | DÉCLENCHEMENT                      |
|      (date/heure incrustées)            |     ON : armé / OFF : chauffe 42 s |
|                                         | ENREGISTREMENT                     |
|      3 images/s pendant un              |   ● REC 12/30 s  /  REC OFF        |
|      enregistrement                     | Fronts 5  Détections 3             |
|                                         | Séquences 3  Fausses 0             |
|                                         | Ignorés 2 (délai)                  |
|                                         | Latence front→image 640 ms         |
|                                         | Dernier 20261014_093012_V0003      |
|                                         | Séq. 30 s, délai 60 s, sens. 3     |
|                                         +------------------------------------+
|                                         | ARMER | VIDÉO | PHOTO | NUIT       |
+-----------------------------------------+------------------------------------+
| OV5647 | 1920x1080 30 i/s 4000 kbit/s son | SD 117.3 Go libres | 15:45:02      |
| état (vert : prêt / résultat ; rouge : REC ou erreur ; jaune : à surveiller) |
| 3 lignes de détail : nom du fichier, bilan, piste de dépannage               |
+------------------------------------------------------------------------------+
```

| Onglet | Contenu |
|---|---|
| **DÉTECTION** | PIR (niveau réel de la broche, rafraîchi toutes les 100 ms), DÉCLENCHEMENT (ON : armé ; OFF : désarmé, chauffe du PIR ou délai entre déclenchements, avec compte à rebours), REC (durée écoulée), compteurs, latence front PIR → 1re image de la dernière séquence, dernier fichier, réglages (pendant REC : images reçues et cadence) |
| **PHOTO** | dernière photo, compte à rebours de la photo de suivi, nombre de photos, taille et durée de la dernière |
| **NUIT** | mode (jour / nuit, forcé ou auto), LED IR (%), filtre IR-cut, couleur / monochrome, lever et coucher du soleil du jour, décision jour/nuit du firmware à cet instant |
| **SON** | vumètre du micro (barre = niveau efficace, trait blanc = crête, de -60 à 0 dBFS), trames son de la dernière vidéo |
| **SYSTÈME** | batterie (ou « non câblée »), température de la puce, carte SD (libre, volume / carte, débit mesuré), mémoire libre, endurance, heure |

Les **boutons tactiles** (bas du panneau) : **ARMER / DÉSARMER**, **VIDÉO** (devient **STOP** pendant un
enregistrement), **PHOTO**, **NUIT / JOUR** (mode forcé). Les onglets se touchent aussi. Un bouton
touché devient blanc un instant. Sans dalle (absente, non détectée ou désactivée dans le menu), tout
reste accessible par le bouton BOOT et la console ; la ligne « Tactile : … » du démarrage l'indique.

## Bouton BOOT

| Geste | Effet |
|---|---|
| appui **court** | armer / désarmer la surveillance PIR |
| appui **long** (1 s) | vidéo manuelle (10 s par défaut) |
| appui pendant un enregistrement ou une boucle d'endurance | arrêt anticipé (fichier fermé proprement) |
| BOOT maintenu pendant un appui sur RESET | mode téléchargement (inchangé) |

## Console série

Dans le moniteur, taper une commande puis Entrée (`help` pour la liste) :

| Commande | Effet |
|---|---|
| `video [durée_s]` / `video stop` | vidéo manuelle / arrêt anticipé |
| `photo` | photo JPEG |
| `armer [on\|off]` | arme / désarme ; sans argument : état du PIR, déclenchement, compteurs |
| `compteurs [raz]` | compteurs de détection et latence ; `raz` : remise à zéro |
| `regle [clé valeur]` | réglages à chaud, syntaxe de `config.ini` : `duree_sequence_s`, `delai_entre_declenchements_s`, `chauffe_pir_s`, `sensibilite`, `intervalle_suivi_min`, `mode`, `latitude`, `longitude`, `avance_crepuscule_min`, `retard_aube_min`, `puissance_ir`, `nuit_monochrome` ; sans argument : valeurs courantes |
| `suivi <min>` | intervalle des photos de suivi (0 = arrêt) |
| `nuit [on\|off\|auto]` | mode nuit forcé, jour forcé, ou décision du firmware (réévaluée chaque minute) |
| `ir <0-100>` / `ircut jour\|nuit` / `mono on\|off` | LED IR, filtre IR-cut, monochrome : commandes directes |
| `soleil` | lever et coucher du soleil, décision jour/nuit du firmware |
| `son` / `son video [s]` | niveau du micro pendant 1 s / vidéo de test du son |
| `batterie` / `temp` | tension de la batterie / température de la puce |
| `sd` / `sd test [Mo]` / `sd scan` | volume de la carte / test de débit et de latence d'écriture / recomptage complet de l'espace libre |
| `boucle N [s]` / `boucle stop` | endurance : N vidéos d'affilée |
| `ecran detection\|photo\|nuit\|son\|systeme` | onglet affiché |
| `fonctions [on\|off <nom> \| go]` | fonctions à tester : liste et état ; activer / désactiver (mémorisé) ; fin de l'écran de sélection |
| `redemarrer` | redémarre la carte (carte SD démontée proprement) |
| `info`, `i2cscan`, `heure [AAAA-MM-JJ HH:MM:SS]` | état complet ; bus I2C ; heure |

Heure : le banc n'a pas d'horloge sauvegardée. Au démarrage, si l'heure n'est pas réglée, il prend
l'heure de compilation du banc ; la régler avec `heure` avant des essais datés (photos de suivi,
lever/coucher du soleil).

## Surveillance PIR : la logique du firmware

Réglages par défaut : ceux du firmware (`nid_config`) — séquence **30 s**, délai **60 s**, chauffe
**60 s**, sensibilité **3** (PIR actif 150 ms au moins), photo de suivi **15 min** ; modifiables dans le
menu « Banc d'essai » (-1 = valeur du firmware) et à chaud par `regle`.

1. **Chauffe** : pendant `chauffe_pir_s` après la mise sous tension, aucun déclenchement
   (« OFF : chauffe 42 s »).
2. **Front montant du PIR** (horodaté dans l'interruption) : l'enregistrement démarre aussitôt ; en
   parallèle, le PIR doit rester actif la durée minimale de la sensibilité. Sinon : **fausse
   détection**, la vidéo est arrêtée et supprimée (le firmware ne l'enregistre pas), compteur
   « Fausses », ligne `fausse_detection` au journal.
3. **Séquence** de `duree_sequence_s`, puis **délai** de `delai_entre_declenchements_s` compté depuis
   la **fin** de la séquence (« OFF : délai 37 s »).
4. Délai à **0** : séquences **enchaînées** tant que le PIR reste actif (10 au plus, puis pause de 60 s).
5. Fronts reçus pendant la chauffe, le délai, un enregistrement, une autre action ou désarmé :
   comptés « Ignorés » avec le motif.
6. **Photo de suivi** toutes les `intervalle_suivi_min` tant que la surveillance est armée.

La **latence** affichée va du front montant du PIR à la première image encodée. Sur le banc elle
comprend l'arrêt du viseur et la reconfiguration de la caméra (≈ 0,3 à 0,8 s attendus) ; sur le
terrain, le réveil et l'initialisation de la caméra remplacent ce temps : la mesure du terrain reste
à faire à l'oscilloscope (FP1.4 : ≤ 1 s).

## Tests dans l'ordre conseillé

| # | Test | Comment | Attendu / interprétation |
|---|---|---|---|
| 1 | Démarrage et sélection | écran de sélection (5 s), puis `i2cscan`, `info`, `fonctions` | fonctions câblées cochées ; ligne « Fonctions : N active(s) » sans « en défaut » ; caméra 0x36 (OV5647) ou 0x30, ES8311 0x18, GT911 0x5D ou 0x14 ; aucun « Failed to register video VFS » ; décocher une fonction (ex. `pir`) : « désactivé » à l'écran, rien ne la sollicite, choix conservé au démarrage suivant |
| 2 | Carte SD | onglet SYSTÈME, `sd`, `sd test` | volume ≈ capacité de la carte (sinon : plusieurs partitions, reformater) ; débit **≥ 4 fois** celui de la vidéo (≥ 2 Mo/s à 4000 kbit/s) ; latence max < 500 ms (sinon `perdues` > 0 probable) ; `sd scan` si l'espace libre semble faux (compteur FSINFO de la carte périmé) |
| 3 | Vidéo avec son | appui long sur BOOT, ou `son video` en parlant | bilan : `trames_son` ≈ durée / 0,064 (≈ 156 pour 10 s), `perdues=0`, `cadence` ≈ réglage ; sur PC : VLC avec son, `ffprobe` : flux `h264` et `aac` 16000 Hz mono |
| 4 | **Détection PIR** | onglet DÉTECTION ; attendre la fin de la chauffe ; passer la main devant le PIR | pastille PIR rouge pendant l'activité (suivre le potentiomètre de temporisation du HC-SR501) ; REC rouge 30 s ; puis « OFF : délai 60 s » ; un 2e passage pendant le délai : « Ignorés (délai) » ; latence < 1 s ; vidéo lisible |
| 4b | Fausse détection | `regle sensibilite 1` (500 ms), effleurer brièvement | « Fausse détection … vidéo supprimée », compteur Fausses +1 |
| 4c | Enchaînement | `regle delai_entre_declenchements_s 0`, rester devant le PIR | séquences enchaînées tant que le PIR est actif |
| 4d | Armer / désarmer | BOOT court, bouton ARMER | DÉSARMÉ : fronts « Ignorés (désarmé) » ; réarmer annule le délai en cours |
| 5 | Photo, suivi | bouton PHOTO ; `suivi 1` | JPEG lisible, date incrustée ; une photo de suivi par minute (onglet PHOTO : compte à rebours) |
| 6 | Nuit | `nuit on`, `ir 50`, `ircut jour/nuit`, `mono on/off`, `nuit auto`, `soleil` | image en niveaux de gris en mode nuit ; LED IR à la puissance demandée (sur la caméra : lueur rouge faible visible au téléphone) ; clic du filtre IR-cut ; lever/coucher cohérents pour la latitude/longitude réglées |
| 7 | Son | onglet SON | barre qui monte en parlant ; silence < -50 dBFS ; crête près de 0 dBFS = saturation |
| 8 | Batterie, température | `batterie`, `temp`, onglet SYSTÈME | sans pont diviseur : « non câblée (… mV lus) », **jamais d'arrêt** ; température 30 à 60 °C selon l'activité |
| 9 | Endurance | `boucle 20` (ou `boucle 50 30`) | `BILAN boucle` : 0 échec, 0 perdue, mémoire interne et PSRAM stables (quelques ko d'écart au plus) ; une baisse à chaque vidéo = fuite |

## Lire les statistiques

À la fin de chaque enregistrement, le moniteur affiche une ligne à copier telle quelle dans un
compte rendu d'essai :

```
I (52341) banc: BILAN video fichier=BANC_N00_20261014_093012_V0001.mp4 motif=PIR resultat=OK erreur=ESP_OK duree=30.00s images=900 cadence=30.0ips perdues=0 trames_son=469 taille=14.91Mo debit=3976kbit/s premiere_image=640ms reglages=1920x1080,30ips,4000kbit/s,son=oui capteur=OV5647
```

| Champ | Signification | Attendu |
|---|---|---|
| `motif` | `manuel`, `PIR`, `PIR enchaînée`, `test son`, `boucle 3/10` | |
| `resultat` | `OK`, `OK_ARRET_ANTICIPE`, `ANNULE` (fausse détection), `ECHEC` (voir `erreur`) | `OK` |
| `duree` | durée de la vidéo (horodatage de la dernière image) | ≈ durée demandée |
| `images` | images encodées | ≈ durée × cadence |
| `cadence` | images / durée | ≈ cadence réglée (diviseur de la cadence du capteur) |
| `perdues` | trous dans la grille des images (carte SD trop lente, processeur trop chargé, capteur plus lent que prévu) | **0** |
| `trames_son` | trames AAC (64 ms chacune à 16 kHz) ; 0 = pas de son | ≈ durée / 0,064 |
| `taille`, `debit` | taille du fichier (1 Mo = 10⁶ octets) et débit moyen réel | débit ≈ réglage (± 20 %) |
| `premiere_image` | du front PIR (motif PIR) ou de la demande (bouton, console) à la 1re image encodée | < 1 s (FP1.4) |

Images perdues : depuis le correctif de l'horodatage (`nid_capture_calc.h`), une image en retard
dans la file de la caméra n'est plus comptée comme perdue. Si `perdues` reste élevé avec
`cadence` nettement sous la cadence réglée, c'est la caméra (ou la boucle d'encodage) qui ne suit
pas : comparer avec `Images reçues … i/s` (onglet DÉTECTION pendant l'enregistrement), essayer
15 i/s, puis sans la vue pendant l'enregistrement (menu).

Autres lignes : `BILAN photo`, `BILAN sd_test` (débit, latence max, blocs > 100 ms, marge par rapport
à la vidéo), `BILAN sd_volume`, `BILAN boucle` (mémoire interne et PSRAM avant → après),
`LATENCE front_pir_premiere_image=…`.

## Fichiers produits

Comme le firmware, avec les identifiants `BANC` / `N00` :

```
/BANC_N00/journal.csv                                   journal (demarrage_banc, video_banc, detection_banc,
                                                        fausse_detection, photo, test_sd_banc, endurance_banc...)
/BANC_N00/20261014/BANC_N00_20261014_093012_V0001.mp4   vidéo
/BANC_N00/20261014/BANC_N00_20261014_093100_P0001.jpg   photo
```

## Réglages (menu « Banc d'essai »)

| Réglage | Défaut |
|---|---|
| Fonctions activées (sous-menu) | toutes ; écran de sélection pendant 5 s au démarrage |
| GPIO du bouton | 35 (BOOT) |
| Durée d'une vidéo manuelle | 10 s |
| Surveillance armée au démarrage | oui |
| Séquence / délai / chauffe / sensibilité / photo de suivi | -1 / -1 / -1 / 0 / -1 = valeurs du firmware (30 s / 60 s / 60 s / 3 / 15 min) |
| Séquences enchaînées au plus | 10 |
| Mode jour/nuit au démarrage | jour forcé |
| Taille du test d'écriture SD | 32 Mo |
| Résolution / images par seconde / débit | 1920x1080 / 30 / 4000 kbit/s (firmware : 15 i/s, 2500 kbit/s) |
| Piste son | oui |
| Qualité JPEG des photos | 80 |
| Fuseau horaire | `CET-1CEST,M3.5.0,M10.5.0/3` (France ; Martinique : `AST4`) |
| Écran | 7 pouces 1024x600 EK79007 (autres choix : ILI9881C 800x1280, aucun) |
| GPIO rétroéclairage / reset de l'écran | 26 / 27 |
| Vue pendant l'enregistrement, images affichées par seconde | oui, 3 |
| Dalle tactile GT911, miroir X / Y, échange X/Y | oui, oui / oui, non (réglage du BSP du kit) |

Les broches de la carte (PIR, I2C, I2S, SD, LED IR, IR-cut, batterie...) sont dans le menu du firmware
« Suivi nid — Carte et brochage », partagé avec le banc. La console série est réglée sur le port
USB Serial/JTAG.

## Dépannage

| Symptôme | Cause probable / solution |
|---|---|
| Écran noir, journal normal | fils GPIO26 (PWM) et GPIO27 (RST_LCD) ; nappe DSI « inversée » ; alimentation USB de l'adaptateur ; menu « Écran » |
| Écran blanc ou rayé | nappe DSI mal enfoncée ou à l'envers ; mauvais modèle d'écran dans le menu |
| Tactile absent | `i2cscan` doit montrer 0x5D ou 0x14 ; nappe de la dalle ; sinon BOOT et console |
| Appui tactile au mauvais endroit | inverser « miroir horizontal / vertical » (menu « Banc d'essai ») |
| `ERREUR caméra` | nappe caméra « directe », contacts face aux contacts, clapets verrouillés ; `i2cscan` : 0x36 (OV5647) ou 0x30 (SC2336) |
| `Failed to register video VFS` | `CONFIG_VFS_MAX_COUNT=16` absent : Full Clean, supprimer `sdkconfig`, recompiler |
| `ERREUR carte SD` | carte absente, mal insérée ou non FAT32 ; insérer la carte puis recommencer |
| Espace libre faux | `sd` : si le volume ne couvre pas la carte, elle a plusieurs partitions (seule la première partition FAT est utilisée) ; `sd scan` recompte le libre et corrige le compteur de la carte |
| PIR toujours ACTIF ou toujours REPOS | câblage OUT → GPIO4, alimentation 5 V, cavalier de mode du HC-SR501, potentiomètres ; le niveau affiché est celui de la broche |
| Aucune séquence sur un front | DÉCLENCHEMENT doit être « ON : armé » (chauffe, délai, désarmé : compteur Ignorés et motif) |
| `perdues` > 0 | `sd test` (carte lente), essayer 15 i/s ou sans la vue pendant l'enregistrement |
| `trames_son=0` ou « SANS SON » | `i2cscan` : 0x18 (ES8311) ; journal `nid_capture_son` ; vumètre de l'onglet SON |
| Batterie « non câblée » | normal sans pont diviseur sur GPIO21 : mesure ignorée, aucun arrêt |
| La console ne répond pas | moniteur ouvert sur le port USB Serial/JTAG ? (la P4X n'a pas de convertisseur USB-série) |

Pour signaler un problème : joindre le journal complet du moniteur depuis le démarrage, une photo de
l'écran, les lignes `BILAN` et, pour une vidéo, la sortie de `ffprobe -show_streams`.

## Organisation du code

| Fichier | Rôle |
|---|---|
| `main/banc_fonctions.c` | fonctions à tester : menu, écran de sélection, NVS, états « désactivé » / « en défaut » |
| `main/banc_main.c` | démarrage (sélection puis initialisation de chaque fonction), tâche d'action (seule à utiliser la caméra), viseur, bouton BOOT, état partagé |
| `main/banc_video.c` | vidéos (manuelle, PIR, endurance), photos, bilans |
| `main/banc_detection.c` | surveillance PIR (logique du firmware), réglages `nid_config`, photos de suivi |
| `main/banc_tests.c` | nuit (LED IR, IR-cut, monochrome, soleil), vumètre, batterie, température, tests de la carte SD |
| `main/banc_ui.c` | onglets, panneau d'état, boutons tactiles |
| `main/banc_ecran.c` | écran MIPI-DSI (pilote `esp_lcd`, sans BSP ni LVGL), vue par le PPA, texte et formes en RGB565 |
| `main/banc_tactile.c` | dalle GT911 (`espressif/esp_lcd_touch_gt911`) |
| `main/banc_console.c` | commandes de la console série |
| `main/Kconfig.projbuild` | menu « Banc d'essai » |

La vue en direct utilise le crochet de débogage `nid_capture_set_frame_hook()` du module de capture ;
le vumètre, `nid_capture_audio_level()` ; la latence, `nid_detect_last_pir_edge_us()`. Ces ajouts
sont sans effet sur le firmware de terrain, qui ne les appelle pas.
