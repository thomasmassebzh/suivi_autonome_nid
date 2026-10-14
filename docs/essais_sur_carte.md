# Essais sur la carte — démarche pas à pas

Objectif : valider le matériel et le logiciel **une fonction à la fois**, du plus simple au plus
complet, pour savoir à chaque étape d'où vient un problème.

Principe : pour chaque fonction, on lance d'abord **l'exemple Espressif** correspondant (code de
référence, connu pour marcher sur cette carte), puis **notre code**. Si l'exemple marche et pas notre
code, le défaut est chez nous. Si l'exemple échoue aussi, c'est le câblage, le matériel ou un réglage.

| Étape | Ce qu'on valide | Programme |
|---|---|---|
| 0 | Assemblage, alimentation | — |
| 1 | Compilation, flash, moniteur | exemple `hello_world` |
| 2 | Bus I2C (caméra, codec audio, écran tactile, horloge) | exemple `i2c_tools` |
| 3 | Carte microSD | exemple `sdmmc` |
| 4 | Écran | exemple `mipi_dsi` |
| 5 | Caméra → écran | exemple `mipi_isp_dsi` |
| 6 | Bibliothèque vidéo utilisée par le projet | exemples `esp_video` : `capture_stream`, `image_storage/sd_card` |
| 7 | **Vidéo sur appui du bouton BOOT (notre code)** | **banc d'essai du projet** (`banc_essai/`) |
| 8 | Micro et haut-parleur | exemple `i2s_es8311` |
| 9 | Wi-Fi (coprocesseur ESP32-C6) | exemple `station` |
| 10 et + | Photo, nuit/IR, PIR, horloge, batterie, veille, page web | banc d'essai, enrichi au fur et à mesure |

Les étapes 8 et 9 ne bloquent pas la 7 : on peut les faire plus tard.

---

## Étape 0 — Assembler la carte

Guide officiel : *ESP32-P4X-Function-EV-Board User Guide* (esp-dev-kits). Points importants :

- **Caméra** : nappe « sens direct » (bandes de contact **du même côté** aux deux bouts).
- **Écran** : nappe « sens inverse » (bandes de contact **de côtés opposés**) entre le connecteur
  MIPI DSI de la carte et J3 de la carte d'adaptation de l'écran.
- **Fils Dupont** entre J1 (carte) et J6 (adaptateur écran) : GPIO27 → `RST_LCD`, GPIO26 → `PWM`
  (rétroéclairage), et 5V → 5V, GND → GND (ou alimenter l'adaptateur par son propre câble USB,
  recommandé par Espressif).
- **USB** : utiliser le port **USB Serial/JTAG** (USB-C) pour flasher et lire le moniteur ; si le
  câble ne fournit pas assez de courant (écran + caméra), brancher en plus une alimentation sur un
  autre port USB-C.
- Carte microSD formatée en **FAT32**.
- Puce rév. v3.1 : **ne jamais activer le « Secure Download Mode »** (errata ROM-770 d'Espressif).

## Créer un projet à partir d'un exemple Espressif (VS Code)

**Exemples d'ESP-IDF** (étapes 1 à 5, 8, 9) :
1. `ESP-IDF: New Project` › choisir l'installation **ESP-IDF v6.1** › choisir l'exemple dans la liste
   (par ex. `get-started/hello_world`) › choisir un dossier **hors du dépôt** (par ex. `~/essais_esp/`).
2. Ouvrir ce dossier, puis comme pour le firmware : `ESP-IDF: Set Espressif Device Target` ›
   **esp32p4**, `ESP-IDF: SDK Configuration Editor` si un réglage est indiqué ci-dessous,
   `ESP-IDF: Select Flash Method` › **UART** (pas JTAG : la méthode JTAG passe par OpenOCD et un pilote
   WinUSB, et échoue avec `esp_usb_jtag: could not find or open device`), port COMx de la carte,
   `ESP-IDF: Build, Flash and Start a Monitor on Your Device` (voir les étapes 3 à 6 du
   [README](../README.md)).

**Exemples de composants du registre** (étape 6, `esp_video`) :
`ESP-IDF: Show ESP Component Registry` › rechercher `esp_video` › onglet des exemples ›
**Create project from this example**. En ligne de commande (terminal `ESP-IDF: Open ESP-IDF Terminal`) :

```sh
idf.py create-project-from-example "espressif/esp_video=2.5.0:capture_stream"
idf.py create-project-from-example "espressif/esp_video=2.5.0:image_storage/sd_card"
```

(version 2.5.0 = celle utilisée par le projet). Tous ces exemples se compilent par défaut pour la
puce rév. v3.1 : ne pas cocher `ESP32P4_SELECTS_REV_LESS_V3`.

---

## Étape 1 — `get-started/hello_world`

Valide : environnement, câble, port, flash, moniteur. Résultat attendu dans le moniteur :
`This is esp32p4 chip with 2 CPU core(s)`, **`silicon revision v3.1`** (ou 3.x), `2MB external flash`
précédé de l'avertissement **`Detected size(16384k) larger than the size in the binary image header(2048k)`** :
les 16 Mo sont bien détectés, l'exemple étant compilé pour 2 Mo par défaut. Si le flash échoue :
vérifier la méthode **UART** (pas JTAG), puis maintenir **BOOT**, appuyer sur **RESET**, relâcher
BOOT, et relancer le flash.

## Étape 2 — `peripherals/i2c/i2c_tools`

Valide : le bus I2C partagé (caméra, codec ES8311, écran tactile, plus tard l'horloge DS3231).
**Réglage obligatoire** : `SDK Configuration Editor` › rechercher *Channel for console output*
(menu *ESP-STDIO*) › **USB Serial/JTAG Controller**. La P4X n'a pas de convertisseur USB-série : par
défaut la console attend les commandes sur l'UART0 (GPIO37/38) et ce qui est tapé dans le moniteur
ne serait pas reçu.
Dans le moniteur, au prompt `i2c-tools>` :

```text
i2cconfig --sda=7 --scl=8 --freq=100000
i2cdetect
```

Adresses attendues : **0x18** codec ES8311, **0x30** caméra SC2336 (kit) ou **0x36** OV5647,
**0x5d** ou **0x14** contrôleur tactile GT911 (écran branché), **0x68** horloge DS3231/DS1307
(quand elle sera câblée). Une caméra absente de la liste = nappe mal insérée ou à l'envers.

## Étape 3 — `storage/sd_card/sdmmc`

Valide : lecteur microSD (alimenté par le régulateur interne n° 4 de l'ESP32-P4).
Dans `SDK Configuration Editor` › *SD/MMC Example Configuration* : cocher
**« SD power supply comes from internal LDO IO »**, LDO ID **4** (les broches par défaut pour
l'ESP32-P4, 39 à 44 en 4 bits, sont les bonnes).
**Laisser décochés** « Format the card if mount failed » et « Format the card as a part of the
example » (sinon la carte est effacée). Résultat attendu : caractéristiques de la carte affichées,
puis écriture et relecture d'un fichier.

## Étape 4 — `peripherals/lcd/mipi_dsi`

Valide : écran, nappe, fils RST/PWM. L'exemple ne commande ni le rétroéclairage ni le reset : dans
`main/mipi_dsi_lcd_example_main.c`, passer `EXAMPLE_PIN_NUM_BK_LIGHT` de `-1` à **`26`** et
`EXAMPLE_PIN_NUM_LCD_RST` de `-1` à **`27`**. Écran **EK79007** (choix par défaut, 1024×600). Résultat
attendu : un widget LVGL s'affiche. Écran noir : vérifier le fil GPIO26 → PWM (rétroéclairage) et
l'alimentation 5 V de l'adaptateur ; écran allumé mais vide : fil GPIO27 → RST_LCD, sens de la nappe.

## Étape 5 — `peripherals/camera/mipi_isp_dsi`

Valide : caméra → ISP → écran, sans la bibliothèque `esp_video`. Dans `SDK Configuration Editor`,
activer le capteur : `CONFIG_CAMERA_SC2336` (caméra du kit) ou `CONFIG_CAMERA_OV5647`. Résultat
attendu : image en direct à l'écran et, dans le moniteur, `Detected Camera sensor PID=...`.
L'image n'est pas réglée (couleurs, netteté) : seul le fonctionnement compte ici. Cet exemple ne
commande pas le rétroéclairage : si l'écran reste noir alors que l'étape 4 marchait, brancher pour
l'essai le fil `PWM` de J6 sur une broche 3V3 de J1 au lieu de GPIO26, puis le remettre.

## Étape 6 — exemples `esp_video` (bibliothèque utilisée par notre module `nid_capture`)

Dans `SDK Configuration Editor`, choisir la carte **ESP32-P4-Function-EV-Board V1.5** (brochage
caméra identique à la P4X, à confirmer à cette étape) et le capteur.

- **`capture_stream`** : le journal affiche le capteur détecté (`sc2336: Detected Camera sensor
  PID=0xcb3a`), le format et la cadence obtenue sur quelques secondes.
- **`image_storage/sd_card`** : c'est l'exemple dont notre module de capture est dérivé. Choisir
  *Video encoding format* › **H.264**. Il enregistre 3 s de vidéo dans `/sdcard/..._h264.bin`, à
  lire sur PC avec VLC. Si cet exemple marche et pas le banc d'essai, le problème vient de notre
  code (mise en MP4, son, incrustation...).

## Étape 7 — banc d'essai du projet : vidéo sur appui du bouton BOOT

Projet séparé `banc_essai/` du dépôt : ouvrir **ce dossier** dans VS Code (et non `firmware/`),
cible esp32p4, compiler, flasher, moniteur. Mode d'emploi détaillé : [banc_essai/README.md](../banc_essai/README.md).

En bref : l'écran affiche la vue caméra et l'état ; un appui sur **BOOT** enregistre une vidéo de
10 s sur la carte SD (même chemin que le firmware de terrain : fichier temporaire, puis nom final),
puis l'écran et le moniteur affichent les statistiques. Un second appui pendant l'enregistrement
l'arrête.

À vérifier :
1. Le fichier `.mp4` existe sur la carte (dossier du jour) et **se lit dans VLC** (image + son).
2. Date et heure incrustées lisibles ; durée de la vidéo ≈ durée demandée.
3. Statistiques : cadence effective proche de la cadence demandée, **0 image perdue**, débit
   proche du réglage, délai de la première image court (< 1 s visé pour le cahier des charges).
4. Sur PC, `ffprobe -show_streams fichier.mp4` (FFmpeg) : un flux `h264` et un flux `aac`.
5. Couper l'alimentation **pendant** un enregistrement, puis redémarrer : au démarrage, le banc
   (comme le firmware de terrain) renomme le fichier interrompu en `..._INCOMPLET` et l'indique à
   l'écran ; aucun fichier ne doit porter un nom final sans être lisible.

## Étape 7 bis — tests du banc

Le banc teste ensuite chaque fonction, écran allumé (onglets DÉTECTION, PHOTO, NUIT, SON, SYSTÈME,
tactiles ou commande `ecran`). Détails, valeurs attendues et interprétation :
[banc_essai/README.md](../banc_essai/README.md#tests-dans-lordre-conseillé). Ordre conseillé :

1. **Démarrage** : écran de sélection des fonctions (5 s, ne garder que le matériel branché),
   `i2cscan` (caméra, ES8311 0x18, GT911 0x5D/0x14), `info`, `heure AAAA-MM-JJ HH:MM:SS`.
2. **Carte SD** (onglet SYSTÈME) : `sd` (volume ≈ capacité de la carte), `sd test` (débit, latence max).
3. **Vidéo avec son** : appui long sur BOOT (ou `son video`) ; `trames_son` > 0, `perdues` = 0, piste
   `aac` dans `ffprobe`.
4. **Détection PIR** (onglet DÉTECTION, priorité) : pastille PIR en temps réel, chauffe puis ARMÉ,
   séquence de 30 s sur un passage, délai de 60 s, fausses détections, latence front → 1re image.
5. **Photo** puis **photo de suivi** : `suivi 1`, une photo par minute tant que la surveillance est armée.
6. **Nuit** : `nuit on/off/auto`, `ir 50`, `ircut jour|nuit`, `mono on|off`, `soleil`.
7. **Son** : vumètre de l'onglet SON (parler, taper près du micro).
8. **Batterie et température** : `batterie` (« non câblée » sans pont diviseur), `temp`.
9. **Endurance** : `boucle 20`, puis comparer la mémoire avant/après et les images perdues.

## Étape 8 — `peripherals/i2s/i2s_codec/i2s_es8311`

Valide : codec audio, micro (et haut-parleur). Les broches par défaut pour l'ESP32-P4 sont celles
de la carte (I2C 7/8, I2S MCLK 13, BCLK 12, WS 10, DOUT 9, DIN 11, ampli GPIO53). Mode **echo** :
ce que capte le micro sort dans le haut-parleur (brancher un haut-parleur sur le connecteur prévu),
mode **music** : lecture d'un extrait. Le système de terrain n'utilise que le micro.

## Étape 9 — `wifi/getting_started/station`

Valide : Wi-Fi par le coprocesseur ESP32-C6 (ESP-Hosted, utilisé uniquement pour la page web
d'installation). Renseigner SSID et mot de passe dans `SDK Configuration Editor`. Résultat attendu :
`got ip:...`. Relever dans le journal la **version du firmware du C6** : notre firmware utilise
ESP-Hosted 3.x, un firmware du C6 trop ancien devra être mis à jour (connecteur de programmation du
C6 sur la carte, avec un ESP-Prog ou un adaptateur USB-série).

---

## Tests unitaires

- **Sur PC**, sans carte : `make -C firmware/components/nid_ir/test_host` (calcul lever/coucher
  du soleil, logique du filtre IR).
- **Sur la carte** : l'extension VS Code sait compiler et lancer des tests Unity
  (`ESP-IDF: Unit Test: Build and Flash Unit Test App for Testing`) placés dans les dossiers
  `test/test_*.c` des composants. Nous en ajouterons pour les fonctions faciles à isoler (nommage
  des fichiers, lecture de `config.ini`), une fois les essais manuels passés.

## Remonter un problème

Pour qu'un défaut puisse être corrigé rapidement, joindre :
- l'étape et le programme utilisés (exemple Espressif ou banc d'essai, avec les réglages changés) ;
- le **journal complet du moniteur** depuis le démarrage (copier-coller du terminal VS Code) ;
- une photo de l'écran si quelque chose s'y affiche ;
- pour une vidéo : la sortie de `ffprobe -show_streams` et, si possible, le fichier lui-même.
