/*
 * sdkconfig.h factice : reproduit ce que génère le menuconfig « Suivi nid — Carte et brochage ».
 * Comme ESP-IDF, seule l'option choisie du groupe « Commande du filtre IR-cut » est définie.
 * Choix par -DMOCK_IRCUT_LEVEL (un fil, -DMOCK_DAY_LEVEL=0/1) ou -DMOCK_IRCUT_PULSE ; défaut : aucune.
 */
#pragma once

#define CONFIG_NID_IR_LED_PWM_FREQ_HZ 20000

#if defined(MOCK_IRCUT_LEVEL)
#ifndef MOCK_DAY_LEVEL
#define MOCK_DAY_LEVEL 1
#endif
#define CONFIG_NID_IRCUT_LEVEL 1
#define CONFIG_NID_IRCUT_DAY_LEVEL MOCK_DAY_LEVEL
#elif defined(MOCK_IRCUT_PULSE)
#define CONFIG_NID_IRCUT_PULSE 1
#define CONFIG_NID_IRCUT_PULSE_MS 150
#else
#define CONFIG_NID_IRCUT_NONE 1
#endif
