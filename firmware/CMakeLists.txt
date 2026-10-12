# Firmware « Suivi autonome d'espèce menacée » — caméra de nid sur ESP32-P4
#
# Projet ESP-IDF (v6.1, figée pour tout le projet). Ouvrir CE dossier (firmware/)
# dans VS Code avec l'extension ESP-IDF, puis : cible esp32p4 → Build → Flash.
# Voir ../README.md.

cmake_minimum_required(VERSION 3.22)

# Version du firmware : reprise dans le journal, la page d'installation et les métadonnées.
set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Ne compile que les composants réellement utilisés (main et ses dépendances) :
# compilation plus rapide et binaire plus petit.
idf_build_set_property(MINIMAL_BUILD ON)

project(suivi_nid)
