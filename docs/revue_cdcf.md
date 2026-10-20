# Relecture du code par rapport au cahier des charges

Code relu : branche `claude/eager-curie-1x4yrr`, commit `9540fd0` (13/10/2026). Référence : tableau FCC
version D (25/09/2026). Méthode : quatre relectures indépendantes en lecture seule (cycle de vie et
énergie ; capture vidéo, son et IR ; données, réglages et horloge ; installation Wi-Fi et web), puis
vérification dans le code de chaque défaut bloquant ou majeur retenu ici.

**Limite** : rien n'a encore tourné sur la carte. « Non vérifiable » = à mesurer lors des essais
(voir [essais_sur_carte.md](essais_sur_carte.md)). Les fonctions mécaniques (FC2, FC4, FC5, FC1.3 à
FC1.5) sont hors du logiciel et ne sont pas traitées.

## En bref

- Le firmware couvre toutes les fonctions du CdCF et la plupart des réglages sont conformes
  (durées, délais, photos, son, formats, capacité de stockage).
- **4 critères impératifs (F0) ne sont pas tenus aujourd'hui** : horodatage (FP1.6), aucune perte de
  données en cas de coupure (FP1.7), confirmation de mise en service (FS1.3), aucune lumière visible
  (FC1.1). Deux autres F0 sont partiels (FP1.2 jour/nuit 24 h/24, FC3.4 fin d'énergie).
- La cause principale est connue : le **pilote d'horloge DS3231 n'est pas encore écrit**. Les autres
  défauts sont localisés et corrigeables sans changer l'architecture.
- Aucun débordement, aucune attente infinie, aucune injection dans `config.ini` ni faille XSS trouvés.

## Conformité critère par critère

| Critère | Flex. | Verdict | Commentaire |
|---|---|---|---|
| FP1.1 détection ≥ 90 %, intempestifs ≤ 20 % | F1/F2 | Non vérifiable | Dépend du PIR. Le journal note les fausses détections (mesure possible). La sensibilité logicielle (0 à 500 ms) ne filtre que les parasites : le vrai réglage reste le potentiomètre du PIR. |
| FP1.2 fonctionnement 24 h/24 | **F0** | Partiel | IR 940 nm et filtre IR-cut gérés, mais jour/nuit décidé par l'heure solaire : faux si l'heure est fausse (défaut 1). Filtre « piloté par le module » par défaut : LED et filtre peuvent être en désaccord à l'aube et au crépuscule. |
| FP1.2 ≥ 720p | F1 | Conforme | 1080p par défaut. |
| FP1.2 couleur jour, monochrome nuit | F2 | Conforme | Sur OV5647. Sur la SC2336 du banc, la couleur peut revenir la nuit (défaut 21). |
| FP1.2 œufs dénombrables | **F0** | Non vérifiable | Dépend de l'optique et de l'IR ; exposition de la photo de nuit à stabiliser (défaut 10). |
| FP1.2 ≥ 15 images/s | F2 | Conforme | 15 i/s par défaut ; le compteur d'images perdues est faussé (défaut 8). |
| FP1.3 piste son, son désactivable | F2 | Partiel | AAC 16 kHz mono dans le MP4 ; pertes et décalage pendant les écritures SD (défaut 9). |
| FP1.4 durée 30 s, réglable 1 s à 5 min | F1/F2 | Conforme | |
| FP1.4 délai 1 min, réglable, désactivable | F1 | Conforme | Délai compté **depuis la fin** de la séquence (90 s entre deux déclenchements par défaut) : **à valider avec l'OFB**. |
| FP1.4 ≤ 1 s entre détection et image | F1 | Non vérifiable | Chemin critique alourdi (défaut 22) ; la mesure interne exclut le démarrage : mesurer à l'oscilloscope depuis le front du PIR. |
| FP1.5 photo de suivi toutes les 15 min | F1 | Conforme | Doublons possibles (défaut 15). |
| FP1.6 date et heure sur chaque fichier et dans l'image | **F0** | **Non conforme** | Incrustation correcte, mais l'heure repart de 1970 à chaque coupure et la surveillance démarre quand même (défaut 1). |
| FP1.6 dérive ≤ 1 min entre deux relevés | F1 | Non vérifiable | Exige le DS3231 (≈ 2 s / 14 j). Le quartz 32 kHz de la carte seul est à la limite (25 à 50 s, plus avec la chaleur). |
| FP1.6 identifiants dans chaque nom de fichier | F1 | Partiel | Présents sur MP4/JPEG ; absents de `journal.csv` ; retour silencieux à CAM01/NID01 (défauts 5 et 7). |
| FP1.7 capacité ≥ 2 semaines | F1 | Conforme | ≈ 4,5 Go pour 14 j (30 séquences/j de 30 s à 2,5 Mbit/s + 1 photo/15 min) : une carte de 32 Go suffit 6 fois. |
| FP1.7 0 fichier perdu ou corrompu, y compris coupure | **F0** | **Non conforme** | MP4 sans index s'il est interrompu (défaut 2) ; carte pleine ou en lecture seule non gérée (défaut 5). |
| FP1.8 formats lisibles (MP4 + son, JPEG) | **F0** | Conforme, à valider | À lire sous VLC, Windows et macOS. |
| FP1.8 copie ≤ 15 min | F2 | Conforme | ≈ 1 à 4 min pour 4,5 Go. |
| FS1.2 image en direct pendant l'installation | F1 | Conforme (code) | Aperçu MJPEG sur la page ; un seul téléphone à la fois. |
| FS1.3 confirmation « en fonctionnement » | **F0** | **Non conforme** | « OK » affiché même sans carte, sans caméra ou sans heure ; voyant invisible en pause maintenance (défaut 4). |
| FS1.4 réglages accessibles sans reprogrammation | F1/F2 | Conforme | `config.ini` + page web, valeurs validées ; erreurs de `config.ini` visibles seulement sur le port série (défaut 18). |
| FS2.1 récupérer les données | F2 | Partiel | Carte neuve : réglages et identifiants remis à zéro ; carte exFAT non détectée (défaut 5). |
| FS2.2 renouveler l'énergie | F2 | Partiel | Échange de batterie = heure perdue tant que le DS3231 n'est pas géré (défaut 1). |
| FC1.1 aucune lumière visible, voyants compris | **F0** | **Non conforme** | Clignotements sans opérateur (reprise après défaut, fin automatique de l'installation) ; voyant et Wi-Fi actifs jusqu'à 15 min après le départ (défauts 3, 4, 13). |
| FC1.2 aucun bruit | F1 | Conforme (code) | Filtre IR-cut commuté seulement au changement jour/nuit ; clic à vérifier sur pièce. |
| FC1.6 intervention ≤ 4-5 min | F1 | Partiel | Fin automatique de l'installation à 15 min par défaut ; pas de portail captif (défaut 13). |
| FC3.1 / FC3.2 autonomie, consommation | F1 | Partiel | Veille profonde, LDO coupés, C6 en reset ; réveils inutiles en batterie basse (défaut 17) ; consommation à mesurer. |
| FC3.3 reprise ≤ 5 min après défaut | F1 | Partiel | Chien de garde correct, mais l'état est perdu (défaut 3) et une boucle de redémarrages est possible (défaut 6). |
| FC3.4 arrêt propre, données préservées | **F0** | Partiel | Fichier fermé et carte démontée au seuil critique ; décision sur une seule mesure prise sous charge (défaut 11), écritures SD avant la mesure (défaut 17). |

## Défauts à corriger

### Lot 1 — impératifs (F0) et pertes de données : avant tout essai sur le terrain

1. **Heure non garantie** (FP1.6, FP1.2, FS2.2). `nid_clock.c` est provisoire (pas de DS3231).
   La surveillance démarre même si l'heure n'est pas réglée (`nid_app.c:643`), et en architecture
   « coupure » l'heure n'est relue qu'après les tests de délai : toutes les détections sont ignorées
   et aucune photo n'est prise. → Écrire le pilote DS3231 ; relire l'horloge au tout début du
   démarrage ; refuser la mise en service sans heure valide (motif d'erreur au voyant, message sur la
   page) ; mettre l'heure du téléphone automatiquement à l'ouverture de la page.
2. **MP4 illisible après une coupure** (FP1.7). Un seul fichier, index (`moov`) écrit à la fermeture
   (`nid_capture.c:1488-1493`) : coupure, chute de tension ou carte pleine pendant la séquence → fichier
   `_INCOMPLET` illisible. → Découper en segments de quelques secondes (prévu par esp_muxer) ou
   utiliser le conteneur TS, lisible même tronqué.
3. **État sauvegardé jamais valide pendant un cycle** (FC3.3, FC1.1). `boot_count++`
   (`nid_app.c:809`) et les compteurs de fichiers modifient l'état en mémoire RTC sans recalculer son
   CRC : après un plantage pendant une vidéo, l'état est jugé corrompu et remis à zéro (numéros de
   fichiers repartis à 0001, pause maintenance oubliée, voyant qui clignote au nid). → `nid_state_commit()`
   après chaque modification.
4. **Voyant et confirmation de mise en service** (FS1.3, FC1.1). Les 5 éclats « OK »
   (`nid_app.c:556`) sont émis sans condition, y compris sans opérateur. Le voyant par défaut
   (GPIO46) est alimenté par le régulateur de la carte SD : il s'éteint quand la carte est démontée,
   donc les 2 éclats de la pause maintenance (`nid_app.c:602-604`) sont invisibles. → « OK » seulement
   si carte, caméra, heure et batterie sont bonnes, sinon motif d'erreur ; aucun clignotement hors
   présence de l'opérateur ; voyant sur un GPIO hors 39-48.
5. **Carte SD** (FS2.1, FP1.6, FP1.7). Carte neuve sans `config.ini` : les réglages en mémoire, y
   compris les identifiants, sont remplacés par les valeurs d'usine (`nid_app.c:175-185`). Carte
   exFAT (toutes les cartes de plus de 32 Go à la livraison, et celles formatées par *SD Card
   Formatter*, que le README recommande à tort), en lecture seule ou pleine : non détectée, le voyant
   annonce quand même « OK ». → Conserver les réglages en cours et les écrire sur la carte neuve
   (copie en NVS) ; test d'écriture au montage ; contrôle de l'espace libre avant chaque séquence ;
   corriger le README.
6. **Boucle de redémarrages** (FC3.3, FC3.4). Après un défaut pendant la photo de suivi, la photo est
   retentée au redémarrage avant d'avancer l'échéance (`nid_app.c:709`) : un défaut répétable
   redémarre la carte toutes les 30 s jusqu'à épuisement de la batterie. → Avancer l'échéance avant
   la capture ; compter les défauts consécutifs et suspendre la caméra au-delà de 3 ; après une
   chute de tension, mesurer la batterie avant de monter la carte.
7. **Identifiant refusé en silence** (FP1.6). Un identifiant trop long ou avec un espace
   (`nid_config.c:826`) est ignoré : le système garde CAM01/NID01, comme tous les autres. → Nettoyer
   et tronquer l'identifiant au lieu de le refuser, et le noter au journal.

### Lot 2 — qualité des données et sécurité

8. **Horodatages vidéo** (`nid_capture.c:1590`) : après chaque ralentissement de la carte SD, le
   décalage des images s'accumule (vidéo en retard sur le son) et le nombre d'images perdues est faux.
9. **Son** : la tâche audio attend la fin des écritures vidéo sur la carte (verrou partagé,
   `nid_capture.c:1424` et `1607`) ; au-delà de 224 ms, des échantillons sont perdus sans être comptés
   et le son prend de l'avance. → File d'attente des trames audio vidée par la tâche vidéo.
10. **Exposition de la photo de suivi** : prise après 8 images au réveil, avant que le réglage
    automatique soit stable → risque de photo de nuit trop sombre (œufs non dénombrables, F0).
11. **Arrêt critique sur une seule mesure** de la batterie prise LED IR et caméra allumées
    (`nid_app.c:422`) : arrêt de 6 h, puis définitif en fin de batterie alors qu'il reste de la
    capacité. → Mesurer avant d'allumer, confirmer par 2 mesures.
12. **Point d'accès Wi-Fi** : mot de passe identique sur tous les boîtiers et publié dans le README,
    renvoyé en clair par `/api/config` ; aucune authentification des actions. Une personne à portée
    pendant l'installation peut mettre le système en pause pour 14 jours. → Mot de passe propre à
    chaque boîtier (imprimé sur l'étiquette), WPA2/WPA3, mot de passe masqué.
13. **Durée de l'installation** : 15 min par défaut (`nid_config.c:944`) avec voyant et Wi-Fi actifs
    si l'opérateur part sans valider. → 5 min par défaut, prolongées à chaque action sur la page ;
    démarrage automatique de la surveillance peu après la déconnexion du téléphone.
14. **Jour/nuit sans capteur** : prévoir la photorésistance (broche déjà prévue) ou forcer la nuit
    quand l'heure n'est pas valide.

### Lot 3 — défauts mineurs

15. Photo de suivi en double si elle est prise en avance (`nid_app.c:252`).
16. Attente du repos du PIR (jusqu'à 15 s) avec caméra et carte encore alimentées (`nid_app.c:742`).
17. Batterie basse : le PIR réveille encore le système à chaque passage ; carte montée et journal
    écrit avant la mesure de la batterie.
18. Erreurs de `config.ini` (valeur « 30 s », fichier UTF-16) ignorées sans trace dans `journal.csv`.
19. `journal.csv` sans identifiants dans son nom et sans BOM (accents illisibles dans Excel).
20. Après un changement d'identifiant de nid, les fichiers interrompus de l'ancien dossier ne sont
    jamais récupérés.
21. Monochrome de nuit écrasé par la correction couleur automatique sur la SC2336 (pas sur l'OV5647).
22. Latence de démarrage évitable : bus caméra à 100 kHz, micro initialisé avant la caméra,
    validation du PIR et création du dossier sur le chemin critique.
23. Un seul échec d'encodage arrête la séquence (le fichier est pourtant lisible).
24. Arrêt du son en délai dépassé : tâche et encodeur non libérés (fuite à la séquence suivante).
25. Aperçu : tampon JPEG réutilisé alors que le serveur web peut encore l'envoyer (installation et
    banc seulement).
26. Serveur web : délais de réception jusqu'à 15 s ; course entre la page et le bouton en fin
    d'installation ; changement de fuseau horaire depuis la tâche web non protégé.
27. État en mémoire RTC sans numéro de version lié au firmware (risque après une mise à jour).
28. Réveil PIR avec un état invalide → mode installation au nid au lieu de la reprise.
29. Bandeau « heure à régler » seulement au-delà de 2 min d'écart (FP1.6 exige ≤ 1 min).
30. Test PC de `nid_config` : 1 échec sur 554 (commentaire en fin de ligne) et Makefile absent.
31. `sdkconfig.defaults` : option `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_LOW` inconnue d'ESP-Hosted 3.x.

## Vérifié sans défaut

- Toutes les attentes sont bornées ; le chien de garde est nourri pendant une vidéo de 5 min.
- Chaque chemin de sortie éteint l'IR, ferme les fichiers (fflush, fsync, fclose) et démonte la carte
  avant la veille.
- Tailles de chaînes et de chemins vérifiées ; aucun nom de fichier n'est jamais écrasé ; noms
  portables Windows/macOS.
- `config.ini` : BOM, fins de ligne Windows, lignes trop longues, clés inconnues et valeurs hors
  plage gérées ; aucune injection possible depuis la page web ; pas de faille XSS.
- Encodage : première image clé avec SPS/PPS à chaque séquence, configuration AAC fournie au MP4,
  cache et tampons DMA corrects d'après les sources d'esp_video 2.5.0.
- Un coprocesseur Wi-Fi muet ou incompatible ramène bien le système en surveillance.
- Le crochet de débogage du banc d'essai est sans effet sur le firmware de terrain.

## À valider avec l'OFB

- Délai entre deux déclenchements compté depuis le début ou la fin de la séquence.
- Durée maximale de la fenêtre d'installation (proposition : 5 min, prolongée tant que l'opérateur
  agit).
- Mot de passe Wi-Fi par boîtier, imprimé sur une étiquette.
