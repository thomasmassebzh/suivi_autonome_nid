# Suivi autonome d'espèce menacée — firmware ESP32-P4

Firmware de la caméra de nid autonome du projet (moqueur gorge-blanche, presqu'île de
la Caravelle, client OFB). Il tourne sur la carte **ESP32-P4X-Function-EV-Board**
(architecture « Arch ESP-P4 » retenue) et réalise les fonctions du cahier des charges
(CdCF version D) :

- détection d'un animal par le PIR et enregistrement d'une **vidéo MP4 (H.264 + son AAC)**
  de durée réglable, avec un délai réglable entre deux déclenchements (FP1.1, FP1.3, FP1.4) ;
- **photo JPEG de suivi** du nid à intervalle réglable, même sans détection (FP1.5) ;
- vision de nuit par **LED infrarouges 940 nm** et filtre IR-cut, sans aucune lumière visible (FP1.2, FC1.1) ;
- **date, heure et identifiants incrustés** dans chaque image et dans chaque nom de fichier (FP1.6) ;
- écriture sécurisée sur carte microSD, journal des événements (FP1.7, FP1.8) ;
- **mode installation** : Wi-Fi local, aperçu en direct et réglages sur smartphone, mise à l'heure (FS1.2 à FS1.4) ;
- veille profonde entre deux événements, chien de garde, arrêt propre sur batterie faible (FC3.1 à FC3.4).

## État d'avancement

Le firmware **compile sans avertissement** pour l'ESP32-P4 avec ESP-IDF v6.1 (vérifié avec les
versions figées des composants). Modules :

| Module | État |
|---|---|
| `main` (cycle réveil → action → veille, état conservé en mémoire RTC) | fait |
| `nid_board`, `nid_detect`, `nid_power` | faits : veille profonde, réveils PIR / bouton / minuterie, batterie, chien de garde — à valider sur la carte |
| `nid_config` | fait : lecture et écriture de `config.ini` |
| `nid_ir` | fait : LED 940 nm, filtre IR-cut, lever/coucher du soleil, avec tests sur PC (`make -C firmware/components/nid_ir/test_host`) |
| `nid_install` | fait : Wi-Fi via le C6, page web (aperçu, réglages, heure, maintenance) — à valider sur la carte |
| `nid_capture` | **à terminer** : police et incrustation date/heure faites (`nid_overlay.c`) ; reste la chaîne caméra → H.264 → MP4 + son AAC, la photo JPEG et l'aperçu |
| `nid_storage`, `nid_clock` | **à faire** : interfaces définies (fonctions provisoires) ; montage de la carte SD, nommage, journal ; pilote DS3231/DS1307 |

Tant que ces trois modules ne sont pas terminés, le système démarre, dort et se réveille, mais
n'enregistre aucun fichier.

> Sommaire : [Matériel](#matériel) · [Installer l'environnement](#1-installer-lenvironnement) ·
> [Ouvrir le projet](#2-ouvrir-le-projet) · [Cible et configuration](#3-choisir-la-cible-et-configurer) ·
> [Compiler](#4-compiler) · [Flasher](#5-flasher) · [Moniteur série](#6-moniteur-série) ·
> [Ligne de commande](#compiler-et-flasher-en-ligne-de-commande) · [Mise en service](#mise-en-service-sur-le-terrain) ·
> [Câblage](#câblage-par-défaut) · [Données](#données-produites-sur-la-carte-sd) · [Dépannage](#dépannage) ·
> [Architecture](#architecture-logicielle)

---

## Matériel

| Élément | Référence | Rôle |
|---|---|---|
| Carte principale | Espressif **ESP32-P4X-Function-EV-Board** (puce rév. ≥ 3.1 + ESP32-C6) | calcul, vidéo H.264, Wi-Fi via le C6 |
| Caméra cible | Waveshare RPi IR-CUT Camera (**OV5647**, IR-cut) — équivalent Arducam B0151 | prise de vue jour/nuit |
| Caméra de banc | caméra 2 MP du kit (**SC2336**, sans IR) | essais sur table |
| Détecteur | **HC-SR501** (PIR) — cible : Panasonic EKMB1109111 | réveil sur passage |
| Horloge | **DS3231** recommandé (le DS1307 de l'inventaire fonctionne en 5 V) | heure exacte, dérive ≤ 1 min / 14 j |
| Éclairage | 3 LED **940 nm** + MOSFET | nuit, invisible pour la faune |
| Stockage | microSD endurance, **FAT32** | vidéos, photos, journal, réglages |
| Carte secondaire | Waveshare ESP32-P4-WIFI6 | voir [Dépannage](#dépannage) (révision de puce) |

## Arborescence du dépôt

```
README.md                 ce fichier
firmware/                 projet ESP-IDF (à ouvrir dans VS Code)
├── CMakeLists.txt        projet « suivi_nid »
├── sdkconfig.defaults    configuration par défaut (puce, PSRAM, caméra, Wi-Fi…)
├── partitions.csv        table des partitions (flash 16 Mo)
├── main/                 logique de l'application (réveil → action → veille)
└── components/           modules du firmware (un dossier par module, voir Architecture)
```

---

## 1. Installer l'environnement

Une seule fois par poste.

1. Installer **Visual Studio Code**, puis l'extension **ESP-IDF** d'Espressif
   (`Affichage` › `Extensions`, rechercher « ESP-IDF »). Version 2.x de l'extension.
2. Installer **ESP-IDF v6.1** — c'est la version figée pour tout le projet :
   - palette de commandes (`F1` ou `Ctrl+Maj+P`) › **`ESP-IDF: Open ESP-IDF Install Manager`** ;
   - dans l'ESP-IDF Installation Manager (EIM), installer **v6.1** (cochez au moins la cible `esp32p4`).
     L'installation télécharge la chaîne de compilation RISC-V et l'environnement Python (≈ 30 min).
3. Sélectionner cette version pour le projet : **`ESP-IDF: Select Current ESP-IDF Version`** › v6.1.
4. Vérifier l'installation : **`ESP-IDF: Doctor Command`** (le rapport doit indiquer ESP-IDF v6.1).

> ESP-IDF v6.1 est déjà installée sur les postes de l'équipe : seules les étapes 3 et 4 sont alors nécessaires.
> Ne pas utiliser v6.0 ni v5.x : le firmware utilise des API propres à v6.1 et les composants
> vidéo sont figés pour cette version.

**Linux** : ajouter son utilisateur au groupe `dialout` (`sudo usermod -aG dialout $USER`, puis se
reconnecter) pour accéder au port série. **Windows 10/11** et **macOS** : aucun pilote à installer
(le port USB Serial/JTAG de l'ESP32-P4 est un port série USB standard).

## 2. Ouvrir le projet

`Fichier` › `Ouvrir le dossier…` › choisir le dossier **`firmware/`** du dépôt (celui qui contient
`CMakeLists.txt` et `sdkconfig.defaults`), pas la racine du dépôt.

Facultatif : **`ESP-IDF: Add VS Code Configuration Folder`** crée le dossier `.vscode/` (coloration
et navigation dans le code C). Ce dossier est propre à chaque poste et n'est pas versionné.

## 3. Choisir la cible et configurer

1. **`ESP-IDF: Set Espressif Device Target`** › **`esp32p4`**, puis la configuration OpenOCD
   proposée pour l'ESP32-P4 (« via builtin USB-JTAG »).
2. Facultatif : **`ESP-IDF: SDK Configuration Editor`** (équivalent de `idf.py menuconfig`) pour
   modifier les menus du projet :
   - **Suivi nid — Application** : mode *Terrain* (veille profonde, par défaut) ou *Banc d'essai*
     (toujours éveillé, pratique pour la mise au point), architecture d'alimentation (veille
     profonde ou coupure matérielle Pololu), délai du chien de garde ;
   - **Suivi nid — Carte et brochage** : GPIO du PIR, du bouton, des LED IR, du filtre IR-cut,
     de la mesure batterie… (voir [Câblage](#câblage-par-défaut)) ;
   - **Suivi nid — Horloge temps réel** : DS3231, DS1307 ou détection automatique.

   Cliquer sur **Save** puis fermer l'éditeur.

> **Révision de puce** : la P4X porte une puce rév. ≥ 3.1. Ne **pas** cocher
> *Component config › Hardware Settings › Chip revision › Select ESP32-P4 revisions <3.0*
> (les binaires rév. 1.x et 3.x ne sont pas compatibles).

Les réglages de terrain (durée des séquences, délai entre déclenchements, intervalle des photos,
son, sensibilité, identifiants, Wi-Fi…) ne demandent **pas** de recompiler : ils sont dans le fichier
`config.ini` de la carte SD (voir [Mise en service](#mise-en-service-sur-le-terrain)).

## 4. Compiler

**`ESP-IDF: Build your Project`** (ou l'icône de compilation de la barre d'état).

- La première compilation télécharge depuis le registre Espressif les composants figés
  (`esp_video` 2.5.0, `esp_cam_sensor` 2.6.0, `esp_h264` 1.4.1, `esp_muxer` 1.2.3,
  `esp_audio_codec`, `esp_codec_dev`, `esp_wifi_remote`, `esp_hosted`…) dans
  `firmware/managed_components/` : **une connexion Internet est nécessaire** et la compilation
  dure plusieurs minutes. Les suivantes sont incrémentales.
- Résultat : `firmware/build/suivi_nid.bin` ; l'analyse de taille s'affiche à la fin.
- Après la **première compilation réussie**, versionner le fichier `firmware/dependencies.lock`
  généré (`git add firmware/dependencies.lock`) : il fige les versions exactes de toutes les
  dépendances pour toute l'équipe.

Après une modification de `sdkconfig.defaults` : **`ESP-IDF: Full Clean Project`**, supprimer le
fichier `firmware/sdkconfig`, puis recompiler (le fichier `sdkconfig` n'est généré qu'une fois à
partir des valeurs par défaut).

## 5. Flasher

1. Relier le port **USB Serial/JTAG** de la carte (USB-C) au PC avec un câble de **données**.
2. **`ESP-IDF: Select Port to Use`** › le port de la carte
   (`/dev/ttyACM0` sous Linux, `COMx` sous Windows, `/dev/cu.usbmodem…` sous macOS).
3. **`ESP-IDF: Select Flash Method`** › **`UART`** (esptool par le port série USB ; c'est la méthode
   standard, sans OpenOCD).
4. **`ESP-IDF: Flash your Project`**. La notification passe à « Flash Done ⚡️ » à la fin.

Raccourci : **`ESP-IDF: Build, Flash and Start a Monitor on Your Device`** enchaîne compilation,
flash et moniteur.

> **Carte en veille profonde** (mode terrain) : le port USB disparaît pendant la veille et le flash
> échoue (« No serial data received »). Appuyer sur **RESET** : la carte redémarre en mode
> installation et reste éveillée (15 min par défaut), le flash fonctionne. En dernier recours,
> maintenir **BOOT**, appuyer sur **RESET**, relâcher **BOOT** (mode téléchargement), flasher, puis
> appuyer sur **RESET**.

## 6. Moniteur série

**`ESP-IDF: Monitor Device`** affiche les messages du firmware (en français) ; `Ctrl+]` pour quitter.

En mode terrain, la liaison USB est coupée à chaque mise en veille : pour la mise au point, passer en
mode **Banc d'essai** (menu *Suivi nid — Application*), ou lire la console sur l'UART0 du connecteur
J1 (TX = GPIO37 broche 8, RX = GPIO38 broche 10, GND broche 9) avec un adaptateur USB-série 3,3 V,
qui reste connecté pendant la veille.

## Compiler et flasher en ligne de commande

Alternative à l'interface, dans un terminal ESP-IDF (**`ESP-IDF: Open ESP-IDF Terminal`**) :

```bash
cd firmware
idf.py set-target esp32p4        # une seule fois
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

---

## Mise en service sur le terrain

### Préparer la carte microSD

- Formater en **FAT32** (au-delà de 32 Go, utiliser un outil qui le permet, par exemple
  *SD Card Formatter* ou *guiformat* ; l'exFAT n'est pas pris en charge).
- Au premier démarrage, le firmware crée `config.ini` à la racine avec les valeurs par défaut du CdCF.
  On peut le modifier sur un ordinateur (fichier texte commenté) ou depuis la page d'installation.

Principaux réglages de `config.ini` :

| Clé | Défaut | Plage | Exigence |
|---|---|---|---|
| `id_systeme`, `id_nid` | CAM01, NID01 | 1–15 caractères `A-Z a-z 0-9 _ -` | FP1.6 |
| `fuseau_horaire` | `AST4` (Martinique, UTC-4) | chaîne TZ POSIX (France : `CET-1CEST,M3.5.0,M10.5.0/3`) | FP1.6 |
| `duree_sequence_s` | 30 | 1–300 | FP1.4 |
| `delai_entre_declenchements_s` | 60 | 0–3600 (0 = désactivé) | FP1.4 |
| `resolution`, `images_par_seconde`, `debit_kbps` | 1920x1080, 15, 2500 | 1920x1080 / 1280x960 / 1280x720 ; 10–30 ; 500–10000 | FP1.2 |
| `son` | oui | oui / non | FP1.3 |
| `intervalle_suivi_min` | 15 | 0–1440 (0 = désactivé) | FP1.5 |
| `sensibilite` | 3 | 1 (peu sensible) – 5 (très sensible) | FP1.1 |
| `mode` (nuit) | auto | auto (lever/coucher du soleil) / jour / nuit | FP1.2 |
| `latitude`, `longitude` | 14.77, -60.89 | degrés | FP1.2 |
| `puissance_ir` | 100 | 0–100 % | FP1.2 |
| `seuil_batterie_basse_mv`, `seuil_batterie_critique_mv` | 3450, 3350 | mV | FC3.4 |
| `wifi_ssid`, `wifi_mot_de_passe` | NID-<id_systeme>, moqueur-nid | 8–63 caractères | FS1.2 |
| `duree_max_min` (installation) | 15 | 1–60 | FC1.6 |

### Installer le système (FS1.1 à FS1.4)

1. Insérer la carte SD, brancher la batterie (ou appuyer sur **RESET**) : le système démarre en
   **mode installation** ; le voyant d'état s'allume (c'est le seul moment où il s'allume).
2. Sur le smartphone, se connecter au Wi-Fi **`NID-CAM01`** (mot de passe de `config.ini`), puis
   ouvrir **http://192.168.4.1** :
   - **Aperçu** en direct pour orienter et cadrer le nid (bouton *Éclairage IR* pour cadrer de nuit) ;
   - **Mettre à l'heure** depuis le téléphone (indispensable si l'horloge a perdu l'heure) ;
   - **Réglages** : modifier puis *Enregistrer* (écrit `config.ini`) ;
   - *Photo de test* pour vérifier l'image enregistrée.
3. **Démarrer la surveillance** (bouton de la page, ou appui **court** sur le bouton du boîtier).
   Le système prend une photo de mise en service, le voyant **clignote 5 fois** puis s'éteint :
   le système est en fonctionnement, on peut quitter le nid (FS1.3).
   Sans action, la surveillance démarre d'elle-même au bout de `duree_max_min`.

### Relever les données lors des rondes (FS2.1, FS2.2)

1. Appuyer sur le bouton du boîtier : le système passe en mode installation.
2. Choisir **Pause maintenance** sur la page, ou faire un appui **long** (≥ 3 s) sur le bouton :
   l'enregistrement en cours est fermé proprement, la carte SD est démontée, le voyant clignote
   2 fois lentement. On peut alors retirer la carte et/ou la batterie sans risque.
3. Remettre la carte (et la batterie), appuyer sur le bouton : retour en mode installation,
   vérifier le cadrage, puis **Démarrer la surveillance**.

---

## Câblage par défaut

Connecteur **J1** de l'ESP32-P4X-Function-EV-Board (tous les GPIO sont modifiables dans
*Suivi nid — Carte et brochage*). Alimentations : 3V3 = broches 1 et 17, 5V = broches 2 et 4,
GND = broches 6, 9, 14, 20, 25, 30, 34, 39.

| Fonction | GPIO | Broche J1 | Remarques |
|---|---|---|---|
| PIR HC-SR501 — sortie | **4** | 18 | VCC du PIR sur 5V, sortie 3,3 V ; résistance de rappel 100 kΩ vers GND ; cavalier en position **L**, temporisation minimale |
| Bouton de mise en service | **5** | 16 | vers GND (tirage interne) |
| I2C SDA (DS3231, partagé avec caméra et codec) | **7** | 3 | DS3231 alimenté en 3V3 |
| I2C SCL | **8** | 5 | |
| LED IR 940 nm (grille du MOSFET) | **32** | 26 | MOSFET logique, LED + résistance sur la batterie |
| Filtre IR-cut A / B | 33 / 36 | 29 / 24 | seulement si le module le demande (menuconfig) |
| Mesure batterie (pont diviseur) | **21** | 11 | ex. 100 kΩ / 100 kΩ → rapport 2000 ; sans pont : pas d'arrêt propre logiciel |
| Voyant d'état | 46 | 36 | allumé uniquement en installation (FC1.1) |
| OFF interrupteur Pololu | 47 | 37 | architecture « coupure matérielle » uniquement |
| INT/SQW du DS3231 | — | — | optionnel (architecture « coupure matérielle ») |
| Reset ESP32-C6 / rétroéclairage LCD / ampli audio | 54 / 26 / 53 | — | gérés par le firmware, maintenus coupés hors installation |
| Caméra | MIPI-CSI | connecteur FPC 15 points | voir Dépannage (brochage, nappe) |
| microSD | 39–44 | emplacement de la carte | alimentée par le LDO interne (canal 4) |

---

## Données produites sur la carte SD

```
/config.ini                                        réglages
/CAM01_NID01/journal.csv                           journal des événements
/CAM01_NID01/20261012/CAM01_NID01_20261012_143507_V0042.mp4   séquence vidéo (H.264 + AAC)
/CAM01_NID01/20261012/CAM01_NID01_20261012_143000_P0123.jpg   photo de suivi
```

- Date et heure locales (fuseau de `config.ini`) dans les noms de fichiers et incrustées dans
  chaque image ; un dossier par jour ; identifiants système et nid dans chaque nom (FP1.6).
- Chaque fichier est écrit sous un nom temporaire `.tmp` puis renommé une fois fermé et
  synchronisé : un fichier sans `.tmp` est toujours complet. Si l'énergie est coupée pendant une
  écriture, le fichier interrompu est renommé `…_INCOMPLET.mp4` au démarrage suivant et signalé
  dans le journal ; les fichiers précédents ne sont jamais rouverts en écriture (FP1.7).
- `journal.csv` (séparateur `;`, s'ouvre dans un tableur) : date et heure, événement (`demarrage`,
  `surveillance`, `video`, `photo`, `fausse_detection`, `batterie_critique`, `maintenance`…),
  détails, tension batterie (mV), température (°C, DS3231).
- Lecture : MP4 et JPEG lisibles sur tout ordinateur sans logiciel propriétaire (VLC, lecteur
  intégré de Windows/macOS) (FP1.8).

Estimation du volume (réglages par défaut : 1080p, 15 img/s, 2,5 Mbit/s, séquences de 30 s,
30 événements par jour, une photo toutes les 15 min) : ≈ 9,5 Mo par séquence, soit ≈ 4 Go de
vidéo et ≈ 0,5 Go de photos sur 14 jours. Une carte de 32 ou 64 Go *endurance* laisse une large
marge (FP1.7 ; mesurer le débit réel sur les premières séquences).

---

## Dépannage

| Symptôme | Cause probable / solution |
|---|---|
| `Failed to resolve component` / échec de téléchargement à la compilation | pas d'accès au registre Espressif : vérifier la connexion (proxy), puis `ESP-IDF: Full Clean Project` et recompiler |
| Erreur sur la révision de puce au flash | la puce est en rév. < 3.0 (cas possible de la carte Waveshare) : vérifier avec `esptool chip-id` ; pour une rév. 1.x, cocher *Select ESP32-P4 revisions <3.0* dans une configuration séparée ; ne jamais flasher avec `--force` |
| « carte SD indisponible » dans le moniteur | carte absente, mal insérée ou non FAT32 ; le firmware ne formate jamais une carte |
| Caméra non détectée | nappe à l'envers (contacts face aux contacts du connecteur, clapets verrouillés) ; le connecteur 15 points de la P4X suit le brochage Espressif : vérifier la correspondance avec la caméra Raspberry Pi avant de brancher ; OV5647 : vérifier la fréquence de l'oscillateur du module (24 ou 25 MHz) |
| Vidéos qui s'arrêtent avant la durée prévue | batterie au seuil critique (voir journal) ou carte SD trop lente (utiliser une carte classe A1/V30) |
| Le PIR déclenche sans raison | baisser la sensibilité (potentiomètre du HC-SR501 et `sensibilite` dans `config.ini`), écarter le PIR de l'antenne Wi-Fi et des LED IR ; les `fausse_detection` sont visibles dans le journal |
| Le moniteur se déconnecte toutes les secondes | normal en mode terrain (veille profonde) : utiliser le mode *Banc d'essai* ou l'UART0 de J1 |
| Le flash échoue (« No serial data received ») | la carte dort : appuyer sur RESET, ou BOOT + RESET (voir [Flasher](#5-flasher)) |

---

## Architecture logicielle

Le firmware est découpé en modules ESP-IDF indépendants (`firmware/components/`), chacun avec
une interface documentée (`include/*.h`). La logique d'ensemble est dans `firmware/main/nid_app.c`.

| Module | Rôle | Exigences CdCF |
|---|---|---|
| `main` (`nid_app`, `nid_state`) | cycle réveil → action → veille, enchaînement des séquences, planification des photos, état conservé en mémoire RTC | FP1.4, FP1.5, FC3.1 |
| `nid_board` | brochage, bus I2C partagé, coprocesseur C6, voyant, mise en sécurité des broches | FC1.1, FC1.2, FC3.2 |
| `nid_detect` | PIR (validation logicielle selon la sensibilité), bouton | FP1.1 |
| `nid_capture` | caméra MIPI-CSI, ISP, H.264 matériel, MP4 + son AAC, photo JPEG, incrustation, aperçu | FP1.2, FP1.3, FP1.6, FP1.8, FS1.2 |
| `nid_ir` | LED IR 940 nm, filtre IR-cut, décision jour/nuit (lever/coucher du soleil) | FP1.2, FC1.1 |
| `nid_clock` | horloge DS3231/DS1307, heure système, fuseau | FP1.6 |
| `nid_storage` | carte microSD, nommage, écriture sécurisée, journal | FP1.6, FP1.7 |
| `nid_config` | lecture/écriture de `config.ini` | FS1.4 |
| `nid_power` | causes de réveil, veille profonde, batterie, chien de garde | FC3.1 à FC3.4 |
| `nid_install` | Wi-Fi via le C6, page web d'installation, aperçu MJPEG, réglages, maintenance | FS1.2 à FS1.4, FS2, FC1.6 |

Chemin critique d'un réveil par le PIR (objectif ≤ 1 s jusqu'à la première image, FP1.4) :
les réglages sont conservés en mémoire RTC (aucune lecture de la carte SD avant de démarrer la
caméra), la caméra démarre pendant qu'une seconde tâche valide le déclenchement, monte la carte SD,
relit l'horloge et mesure la batterie.

Choix techniques :

- **Chaîne vidéo** : `esp_video` (V4L2, ISP avec exposition et balance des blancs automatiques)
  → encodeur H.264 matériel → `esp_muxer` (MP4) ; audio `esp_codec_dev` (ES8311) → AAC
  (`esp_audio_codec`). Le composant de haut niveau `esp_capture` n'est pas utilisé : il impose
  `esp-sr` et une version bêta de `esp_codec_dev` (2.0.0-beta1) ; la chaîne directe donne aussi
  la maîtrise du délai de démarrage et de l'écriture des fichiers.
- **Architecture d'alimentation** : la preuve de concept utilise la veille profonde de l'ESP32-P4
  (carte toujours alimentée). L'architecture cible (interrupteur Pololu commandé par le PIR et
  l'alarme du DS3231) est prévue dans le code (option de menuconfig) et reste à valider sur matériel.
