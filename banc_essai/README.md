# Banc d'essai — validation pas à pas sur la carte

Programme de test de l'**ESP32-P4X-Function-EV-Board**, distinct du firmware de terrain. Il compile
directement les modules du firmware (`../firmware/components/nid_board`, `nid_capture`,
`nid_storage`, sans les copier) pour valider sur la carte, une fonction après l'autre, **le vrai
chemin du firmware** : caméra → incrustation date/heure → H.264 + son AAC → MP4 sur la carte SD
(fichier `.tmp` puis nom définitif, `_INCOMPLET` après une coupure).

Étape 1 (actuelle) : **enregistrer une vidéo sur appui du bouton BOOT** et afficher son bilan.
Le guide d'essais complet (exemples Espressif puis banc) est dans [docs/essais_sur_carte.md](../docs/essais_sur_carte.md).

Le banc n'utilise ni veille profonde, ni Wi-Fi, ni PIR, ni LED IR : il reste éveillé et toute erreur
est affichée sans redémarrer la carte.

## Matériel

- ESP32-P4X-Function-EV-Board (puce rév. ≥ 3.1), caméra SC2336 du kit (ou OV5647) sur le
  connecteur MIPI-CSI avec la nappe « directe » (bandes du même côté aux deux bouts) ;
- carte microSD formatée en **FAT32** (le banc ne formate jamais une carte) ;
- écran 7 pouces du kit (1024×600, EK79007), facultatif mais conseillé :
  - nappe « inversée » (bandes de côtés opposés) entre le connecteur MIPI-DSI de la carte et J3 de
    l'adaptateur de l'écran ;
  - fil **GPIO27** (J1 broche 38) → **RST_LCD** (J6 de l'adaptateur) ;
  - fil **GPIO26** (J1 broche 31) → **PWM** (J6), rétroéclairage ;
  - alimentation de l'écran : câble USB sur J1 de l'adaptateur (conseillé), ou 5V/GND depuis J1 de la carte.

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

Après une modification de `sdkconfig.defaults` : **`ESP-IDF: Full Clean Project`**, supprimer
`banc_essai/sdkconfig`, recompiler.

## Utilisation

### Écran

```
+------------------------------------------------------------+
|                                                            |
|          vue caméra en direct (date/heure incrustées)      |
|                                                            |
+------------------------------------------------------------+
| SC2336 | 1920x1080 30 i/s 4000 kbit/s son | SD 29.1 Go libres | 15:45:02
| Prêt — appuyez sur BOOT pour enregistrer 10 s              |  ← état (couleur)
| ...                                                        |  ← résultats ou erreur
+------------------------------------------------------------+
```

- **Vert** : prêt, résultat correct. **Rouge** : enregistrement en cours (« Enregistrement 3/10 s »)
  ou erreur (étape, code d'erreur, piste de dépannage). **Jaune** : à surveiller (images perdues,
  fichiers interrompus retrouvés au démarrage).
- Pendant un enregistrement, la vue n'est rafraîchie que 3 fois par seconde (réglable) pour ne pas
  ralentir l'enregistrement.
- Au démarrage, la ligne « Carte SD » indique combien de fichiers interrompus (coupure pendant une
  écriture) ont été renommés `..._INCOMPLET`.

### Bouton BOOT

- Un appui : enregistre une vidéo (10 s par défaut).
- Un appui **pendant** l'enregistrement : arrêt anticipé, le fichier est fermé proprement.
- Maintenir BOOT pendant un appui sur RESET passe toujours la carte en mode téléchargement.

### Console série

Dans le moniteur, taper une commande puis Entrée (`help` pour la liste) :

| Commande | Effet |
|---|---|
| `video [durée_s]` | enregistre une vidéo (même tâche que le bouton : jamais deux enregistrements à la fois) |
| `photo` | photo JPEG sur la carte SD |
| `info` | capteur, réglages, carte SD, mémoire libre interne/PSRAM, version, dernier bilan |
| `nuit on` / `nuit off` | image en niveaux de gris (mode nuit de l'ISP) / couleur ; les LED IR ne sont pas pilotées |
| `i2cscan` | liste le bus I2C partagé : SC2336 0x30, OV5647 0x36, ES8311 0x18, GT911 0x5D/0x14, DS3231/DS1307 0x68 |
| `heure AAAA-MM-JJ HH:MM:SS` | règle l'heure locale (sans argument : l'affiche) |

Heure : le banc n'a pas d'horloge sauvegardée. Au démarrage, si l'heure n'est pas réglée, il prend
l'heure de compilation du banc (incrustation et noms de fichiers restent lisibles) ; la régler avec
`heure` avant des essais datés.

### Fichiers produits

Comme le firmware, avec les identifiants `BANC` / `N00` :

```
/BANC_N00/journal.csv                                   journal (lignes demarrage_banc, video_banc, photo_banc, erreur...)
/BANC_N00/20261014/BANC_N00_20261014_093012_V0001.mp4   vidéo
/BANC_N00/20261014/BANC_N00_20261014_093100_P0001.jpg   photo
```

## Que vérifier après un enregistrement

1. Couper l'alimentation (ou attendre la fin de l'enregistrement), retirer la carte SD, copier le
   `.mp4` sur un PC.
2. Lecture dans **VLC** : image nette, date et heure incrustées en bas à gauche, son présent,
   durée ≈ durée demandée, image et son synchronisés.
3. `ffprobe -show_streams fichier.mp4` (FFmpeg) : un flux `codec_name=h264` (largeur, hauteur,
   `avg_frame_rate` attendus) et un flux `codec_name=aac` (16000 Hz, mono).
4. Le journal `journal.csv` contient une ligne `video_banc` avec les mêmes chiffres que l'écran.

## Lire les statistiques

À la fin de chaque enregistrement, le moniteur affiche une ligne à copier telle quelle dans un
compte rendu d'essai :

```
I (52341) banc: BILAN video fichier=BANC_N00_20261014_093012_V0001.mp4 resultat=OK erreur=ESP_OK duree=10.00s images=300 cadence=30.0ips perdues=0 trames_son=157 taille=4.91Mo debit=3930kbit/s premiere_image=640ms reglages=1920x1080,30ips,4000kbit/s,son=oui capteur=SC2336
```

| Champ | Signification | Attendu |
|---|---|---|
| `resultat` | `OK`, `OK_ARRET_ANTICIPE` (bouton) ou `ECHEC` (voir `erreur` et l'écran) | `OK` |
| `duree` | durée de la vidéo (horodatage de la dernière image) | ≈ durée demandée |
| `images` | images encodées | ≈ durée × cadence |
| `cadence` | images / durée | ≈ cadence réglée (diviseur de la cadence du capteur) |
| `perdues` | trous dans la suite des images (carte SD trop lente, processeur trop chargé) | **0** |
| `trames_son` | trames AAC (64 ms chacune à 16 kHz) ; 0 = pas de son | ≈ durée / 0,064 |
| `taille`, `debit` | taille du fichier (1 Mo = 10⁶ octets) et débit moyen réel | débit ≈ réglage (± 20 %) |
| `premiere_image` | délai entre l'appui (ou la commande) et la 1re image encodée | < 1 s visé (FP1.4) |

Le délai de la première image inclut ici la reconfiguration de la caméra (passage de la vue en
direct à la vidéo) : il est plus long que sur le terrain, où la caméra démarre directement en vidéo.
Pour vérifier que l'écran ne fait pas perdre d'images, désactiver « Vue caméra pendant
l'enregistrement » (menu « Banc d'essai ») et comparer `perdues`.

## Réglages (menu « Banc d'essai »)

| Réglage | Défaut |
|---|---|
| GPIO du bouton | 35 (BOOT) |
| Durée d'un enregistrement | 10 s |
| Résolution / images par seconde / débit | 1920x1080 / 30 / 4000 kbit/s |
| Piste son | oui |
| Qualité JPEG des photos | 80 |
| Fuseau horaire | `CET-1CEST,M3.5.0,M10.5.0/3` (France ; Martinique : `AST4`) |
| Écran | 7 pouces 1024x600 EK79007 (autres choix : ILI9881C 800x1280, aucun) |
| GPIO rétroéclairage / reset de l'écran | 26 / 27 |
| Vue pendant l'enregistrement, images affichées par seconde | oui, 3 |

Les broches de la carte (I2C, I2S, SD...) sont dans le menu du firmware « Suivi nid — Carte et
brochage », partagé avec le banc. La console série est réglée sur le port USB Serial/JTAG.

## Dépannage

| Symptôme | Cause probable / solution |
|---|---|
| Écran noir, journal normal | fils GPIO26 (PWM) et GPIO27 (RST_LCD) ; nappe DSI « inversée » ; alimentation USB de l'adaptateur ; menu « Écran » |
| Écran blanc ou rayé | nappe DSI mal enfoncée ou à l'envers ; mauvais modèle d'écran dans le menu |
| `ERREUR caméra` | nappe caméra « directe », contacts face aux contacts, clapets verrouillés ; `i2cscan` doit montrer 0x30 (SC2336) ou 0x36 (OV5647) ; appuyer sur BOOT pour réessayer |
| `ERREUR carte SD` | carte absente, mal insérée ou non FAT32 ; insérer la carte puis appuyer sur BOOT (montage réessayé) |
| `ERREUR enregistrement : ESP_ERR_TIMEOUT` | aucune image de la caméra : nappe, alimentation ; voir les lignes `nid_capture` du journal |
| `ERREUR enregistrement : ESP_FAIL` | écriture impossible : carte pleine, retirée ou trop lente (classe A1/V30) |
| `perdues` > 0 | carte SD trop lente, débit trop élevé ; recommencer sans la vue pendant l'enregistrement |
| `trames_son=0` | codec ES8311 non détecté (`i2cscan` : 0x18) ; voir le journal `nid_capture_son` |
| La console ne répond pas | moniteur ouvert sur le port USB Serial/JTAG ? (la P4X n'a pas de convertisseur USB-série) |
| `heure` donne une date de compilation | normal sans horloge : régler l'heure avec `heure AAAA-MM-JJ HH:MM:SS` |

Pour signaler un problème : joindre le journal complet du moniteur depuis le démarrage, une photo de
l'écran, la ligne `BILAN` et, pour une vidéo, la sortie de `ffprobe -show_streams`.

## Organisation du code

| Fichier | Rôle |
|---|---|
| `main/banc_main.c` | démarrage, tâche d'action (vidéo, photo, viseur), bouton BOOT, bilans |
| `main/banc_ecran.c` | écran MIPI-DSI (pilote `esp_lcd`, sans BSP ni LVGL), conversion des images par le PPA, texte du bandeau |
| `main/banc_console.c` | commandes de la console série |
| `main/Kconfig.projbuild` | menu « Banc d'essai » |

La vue en direct utilise le crochet de débogage `nid_capture_set_frame_hook()` du module de capture :
chaque image capturée (aperçu, vidéo, photo) est remise au banc après incrustation et encodage. Le
firmware de terrain n'installe pas de crochet (aucun coût).
