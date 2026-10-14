# Banc d'essai « Suivi autonome d'espèce menacée » — ESP32-P4X-Function-EV-Board
#
# Projet ESP-IDF (v6.1) DISTINCT du firmware de terrain : ouvrir CE dossier
# (banc_essai/) dans VS Code avec l'extension ESP-IDF, puis : cible esp32p4 →
# Build → Flash → Monitor. Voir README.md.
#
# Le banc ne copie aucun module : il compile directement les composants du
# firmware (../firmware/components) pour valider sur la carte, pas à pas, le
# vrai chemin du firmware (caméra, encodage, carte SD).

cmake_minimum_required(VERSION 3.22)

set(PROJECT_VER "0.1.0")

# Composants du firmware utilisés par le banc (leurs propres dépendances
# suivent par REQUIRES / PRIV_REQUIRES et par leurs idf_component.yml).
set(BANC_COMPOSANTS_FIRMWARE nid_board nid_capture nid_storage nid_config nid_detect nid_ir nid_power nid_clock)
get_filename_component(NID_COMPOSANTS "${CMAKE_CURRENT_LIST_DIR}/../firmware/components" ABSOLUTE)
foreach(composant ${BANC_COMPOSANTS_FIRMWARE})
    list(APPEND EXTRA_COMPONENT_DIRS "${NID_COMPOSANTS}/${composant}")
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Ne compile que main et ses dépendances.
idf_build_set_property(MINIMAL_BUILD ON)

project(banc_essai)
