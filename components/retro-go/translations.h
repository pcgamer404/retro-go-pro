#include "rg_localization.h"

static const char *language_names[RG_LANG_MAX] = {
    [RG_LANG_EN] = "English",
    [RG_LANG_FR] = "Francais",
    [RG_LANG_DE] = "Deutsch",
};

static const char *translations[][RG_LANG_MAX] =
{
    {
        [RG_LANG_EN] = "Never",
        [RG_LANG_FR] = "Jamais",
        [RG_LANG_DE] = "Niemals",
    },
    {
        [RG_LANG_EN] = "Always",
        [RG_LANG_FR] = "Toujours",
        [RG_LANG_DE] = "Immer",
    },
    {
        [RG_LANG_EN] = "Composite",
        [RG_LANG_FR] = "Composite",
        [RG_LANG_DE] = "Composite",
    },
    {
        [RG_LANG_EN] = "NES Classic",
        [RG_LANG_FR] = "NES Classic",
        [RG_LANG_DE] = "NES Classic",
    },
    {
        [RG_LANG_EN] = "NTSC",
        [RG_LANG_FR] = "NTSC",
        [RG_LANG_DE] = "NTSC",
    },
    {
        [RG_LANG_EN] = "PVM",
        [RG_LANG_FR] = "PVM",
        [RG_LANG_DE] = "PVM",
    },
    {
        [RG_LANG_EN] = "Smooth",
        [RG_LANG_FR] = "Lisser",
        [RG_LANG_DE] = "Glatt",
    },
    {
        [RG_LANG_EN] = "To start, try: 1 or * or #",
        [RG_LANG_FR] = "Pour commencer, 1 ou * ou #",
        [RG_LANG_DE] = "Zum Starten, versuche 1 oder * oder #",
    },
    {
	    [RG_LANG_EN] = "Full",
	    [RG_LANG_FR] = "Complet",
        [RG_LANG_DE] = "Voll",
    },
    {
        [RG_LANG_EN] = "Yes",
        [RG_LANG_FR] = "Oui",
        [RG_LANG_DE] = "Ja",
    },
    {
        [RG_LANG_EN] = "Select file",
        [RG_LANG_FR] = "Choisissez un fichier",
        [RG_LANG_DE] = "Datei auswählen",
    },
    {
        [RG_LANG_EN] = "Language",
        [RG_LANG_FR] = "Langue",
        [RG_LANG_DE] = "Sprache",
    },
    {
        [RG_LANG_EN] = "Language changed!",
        [RG_LANG_FR] = "Changement de langue",
        [RG_LANG_DE] = "Sprache geändert!",
    },
    {
        [RG_LANG_EN] = "For these changes to take effect you must restart your device.\nrestart now?",
        [RG_LANG_FR] = "Pour que ces changements prennent effet, vous devez redémarrer votre appareil.\nRedémarrer maintenant ?",
        [RG_LANG_DE] = "Um die Änderungen anzuwenden muss das Gerät neu gestartet werden. \nJetzt neustarten?",
    },
    {
        [RG_LANG_EN] = "Wi-Fi profile",
        [RG_LANG_FR] = "Profil Wi-Fi",
        [RG_LANG_DE] = "Wi-Fi Profil",
    },
    {
        [RG_LANG_EN] = "Language",
        [RG_LANG_FR] = "Langue",
        [RG_LANG_DE] = "Sprache",
    },
    {
        [RG_LANG_EN] = "Options",
        [RG_LANG_FR] = "Options",
        [RG_LANG_DE] = "Optionen",
    },
    {
        [RG_LANG_EN] = "About Retro-Go",
        [RG_LANG_FR] = "A propos de Retro-go",
        [RG_LANG_DE] = "Über Retro-Go",
    },
    {
        [RG_LANG_EN] = "Reset all settings?",
        [RG_LANG_FR] = "Reset tous les paramètres",
        [RG_LANG_DE] = "Alle Einstellungen zurücksetzen?",
    },
    {
        [RG_LANG_EN] = "Initializing...",
        [RG_LANG_FR] = "Initialisation...",
        [RG_LANG_DE] = "Initialisierung...",
    },
    {
        [RG_LANG_EN] = "Host Game (P1)",
        [RG_LANG_FR] = "Host Game (P1)",
        [RG_LANG_DE] = "Hoste Spiel (P1)",
    },
    {
        [RG_LANG_EN] = "Find Game (P2)",
        [RG_LANG_FR] = "Find Game (P2)",
        [RG_LANG_DE] = "Finde Spiel (P2)",
    },
    {
        [RG_LANG_EN] = "Netplay",
        [RG_LANG_FR] = "Netplay",
        [RG_LANG_DE] = "Netplay",
    },
    {
        [RG_LANG_EN] = "ROMs not identical. Continue?",
        [RG_LANG_FR] = "ROMs not identical. Continue?",
        [RG_LANG_DE] = "ROMs nicht identisch. Weiter?",
    },
    {
        [RG_LANG_EN] = "Exchanging info...",
        [RG_LANG_FR] = "Exchanging info...",
        [RG_LANG_DE] = "Informationsaustausch...",
    },
    {
        [RG_LANG_EN] = "Unable to find host!",
        [RG_LANG_FR] = "Unable to find host!",
        [RG_LANG_DE] = "Kein Host gefunden!",
    },
    {
        [RG_LANG_EN] = "Connection failed!",
        [RG_LANG_FR] = "Connection failed!",
        [RG_LANG_DE] = "Verbindung fehlgeschlagen!",
    },
    {
        [RG_LANG_EN] = "Waiting for peer...",
        [RG_LANG_FR] = "Waiting for peer...",
        [RG_LANG_DE] = "Warte auf peer",
    },
    {
        [RG_LANG_EN] = "Unknown status...",
        [RG_LANG_FR] = "Unknown status...",
        [RG_LANG_DE] = "Unbekannter Status...",
    },
    {
        [RG_LANG_EN] = "On",
        [RG_LANG_FR] = "On",
        [RG_LANG_DE] = "An",
    },
    {
        [RG_LANG_EN] = "Keyboard",
        [RG_LANG_FR] = "Clavier",
        [RG_LANG_DE] = "Keyboard",
    },
    {
        [RG_LANG_EN] = "Joystick",
        [RG_LANG_FR] = "Joystick",
        [RG_LANG_DE] = "Joystick",
    },
    {
        [RG_LANG_EN] = "Input",
        [RG_LANG_FR] = "Entrée",
        [RG_LANG_DE] = "Eingabe",
    },
    {
        [RG_LANG_EN] = "Crop",
        [RG_LANG_FR] = "Couper",
        [RG_LANG_DE] = "Zuschneiden",
    },
    {
        [RG_LANG_EN] = "BIOS file missing!",
        [RG_LANG_FR] = "Fichier BIOS manquant",
        [RG_LANG_DE] = "BIOS-Datei fehlt!",
    },
    {
        [RG_LANG_EN] = "YM2612 audio ",
        [RG_LANG_FR] = "YM2612 audio ",
        [RG_LANG_DE] = "YM2612 audio ",
    },
    {
        [RG_LANG_EN] = "SN76489 audio",
        [RG_LANG_FR] = "SN76489 audio",
        [RG_LANG_DE] = "SN76489 audio",
    },
    {
        [RG_LANG_EN] = "Z80 emulation",
        [RG_LANG_FR] = "Emulation Z80",
        [RG_LANG_DE] = "Z80 emulation",
    },
    {
        [RG_LANG_EN] = "Launcher options",
        [RG_LANG_FR] = "Options du lanceur",
        [RG_LANG_DE] = "Launcher Optionen",
    },
    {
        [RG_LANG_EN] = "Emulator options",
        [RG_LANG_FR] = "Options émulateur",
        [RG_LANG_DE] = "Emulator Optionen",
    },
    {
        [RG_LANG_EN] = "Date",
        [RG_LANG_FR] = "Date",
        [RG_LANG_DE] = "Datum",
    },
    {
        [RG_LANG_EN] = "Files:",
        [RG_LANG_FR] = "Fichiers:",
        [RG_LANG_DE] = "Dateien",
    },
    {
        [RG_LANG_EN] = "Download complete!",
        [RG_LANG_FR] = "Téléchargement terminé",
        [RG_LANG_DE] = "Download abgeschlossen!",
    },
    {
        [RG_LANG_EN] = "Reboot to flash?",
        [RG_LANG_FR] = "Redémarrer",
        [RG_LANG_DE] = "Neustarten um zu flashen",
    },
    {
        [RG_LANG_EN] = "Available Releases",
        [RG_LANG_FR] = "Maj disponible",
        [RG_LANG_DE] = "Verfügbare Releases",
    },
    {
        [RG_LANG_EN] = "Received empty list!",
        [RG_LANG_FR] = "Liste vide reçue",
        [RG_LANG_DE] = "Leere Liste empfangen!",
    },
    {
        [RG_LANG_EN] = "Gamma Boost",
        [RG_LANG_FR] = "Boost Gamma",
        [RG_LANG_DE] = "Gamma Boost",
    },
    {
        [RG_LANG_EN] = "Day",
        [RG_LANG_FR] = "Jour",
        [RG_LANG_DE] = "Tag",
    },
    {
        [RG_LANG_EN] = "Hour",
        [RG_LANG_FR] = "Heure",
        [RG_LANG_DE] = "Stunde",
    },
    {
        [RG_LANG_EN] = "Min",
        [RG_LANG_FR] = "Min",
        [RG_LANG_DE] = "Min",
    },
    {
        [RG_LANG_EN] = "Sec",
        [RG_LANG_FR] = "Sec",
        [RG_LANG_DE] = "Sek",
    },
    {
        [RG_LANG_EN] = "Sync",
        [RG_LANG_FR] = "Synchro",
        [RG_LANG_DE] = "Synchronisieren",
    },
    {
        [RG_LANG_EN] = "RTC config",
        [RG_LANG_FR] = "Config RTC",
        [RG_LANG_DE] = "RTC Einstellung",
    },
    {
        [RG_LANG_EN] = "Palette",
        [RG_LANG_FR] = "Palette",
        [RG_LANG_DE] = "Palette",
    },
    {
        [RG_LANG_EN] = "RTC config",
        [RG_LANG_FR] = "Config RTC",
        [RG_LANG_DE] = "RTC Einstellung",
    },
    {
        [RG_LANG_EN] = "SRAM autosave",
        [RG_LANG_FR] = "Sauvegarde auto SRAM",
        [RG_LANG_DE] = "SRAM autospeichern",
    },
    {
        [RG_LANG_EN] = "Enable BIOS",
        [RG_LANG_FR] = "Activer BIOS",
        [RG_LANG_DE] = "BIOS aktivieren",
    },
    {
        [RG_LANG_EN] = "Name",
        [RG_LANG_FR] = "Nom",
        [RG_LANG_DE] = "Name",
    },
    {
        [RG_LANG_EN] = "Artist",
        [RG_LANG_FR] = "Artiste",
        [RG_LANG_DE] = "Künstler",
    },
    {
        [RG_LANG_EN] = "Copyright",
        [RG_LANG_FR] = "Copyright",
        [RG_LANG_DE] = "Copyright",
    },
    {
        [RG_LANG_EN] = "Playing",
        [RG_LANG_FR] = "Playing",
        [RG_LANG_DE] = "Spielt",
    },
    {
        [RG_LANG_EN] = "Palette",
        [RG_LANG_FR] = "Palette",
        [RG_LANG_DE] = "Palette",
    },
    {
        [RG_LANG_EN] = "Overscan",
        [RG_LANG_FR] = "Overscan",
        [RG_LANG_DE] = "Overscan",
    },
    {
        [RG_LANG_EN] = "Crop sides",
        [RG_LANG_FR] = "Couper les côtés",
        [RG_LANG_DE] = "Seiten abschneiden",
    },
    {
        [RG_LANG_EN] = "Sprite limit",
        [RG_LANG_FR] = "Limite de sprite",
        [RG_LANG_DE] = "Sprite Limit",
    },
    {
        [RG_LANG_EN] = "Overscan",
        [RG_LANG_FR] = "Overscan",
        [RG_LANG_DE] = "Overscan",
    },
    {
        [RG_LANG_EN] = "Palette",
        [RG_LANG_FR] = "Palette",
        [RG_LANG_DE] = "Palette",
    },
    {
        [RG_LANG_EN] = "Profile",
        [RG_LANG_FR] = "Profil",
        [RG_LANG_DE] = "Profil",
    },
    {
        [RG_LANG_EN] = "<profile name>",
        [RG_LANG_FR] = "<nom du profil>",
        [RG_LANG_DE] = "<profil name>",
    },
    {
        [RG_LANG_EN] = "Controls",
        [RG_LANG_FR] = "Contrôles",
        [RG_LANG_DE] = "Steuerung",
    },
    {
        [RG_LANG_EN] = "Audio enable",
        [RG_LANG_FR] = "Activer audio",
        [RG_LANG_DE] = "Audio einschalten",
    },
    {
        [RG_LANG_EN] = "Audio filter",
        [RG_LANG_FR] = "Filtre audio",
        [RG_LANG_DE] = "Audio Filter",
    },


    // rg_gui.c
    {
        [RG_LANG_EN] = "Folder is empty.",
        [RG_LANG_FR] = "Le dossier est vide.",
        [RG_LANG_DE] = "Ordner ist leer",
    },
    {
        [RG_LANG_EN] = "No",
        [RG_LANG_FR] = "Non",
        [RG_LANG_DE] = "Nein",
    },
    {
        [RG_LANG_EN] = "OK",
        [RG_LANG_FR] = "OK",
        [RG_LANG_DE] = "OK",
    },
    {
        [RG_LANG_EN] = "On",
        [RG_LANG_FR] = "On",
        [RG_LANG_DE] = "An",
    },
    {
        [RG_LANG_EN] = "Off",
        [RG_LANG_FR] = "Off",
        [RG_LANG_DE] = "Aus",
    },
    {
        [RG_LANG_EN] = "Horiz",
        [RG_LANG_FR] = "Horiz",
        [RG_LANG_DE] = "Horiz",
    },
    {
        [RG_LANG_EN] = "Vert",
        [RG_LANG_FR] = "Vert ",
        [RG_LANG_DE] = "Vert",
    },
    {
        [RG_LANG_EN] = "Both",
        [RG_LANG_FR] = "Tout",
        [RG_LANG_DE] = "Beide",
    },
    {
        [RG_LANG_EN] = "Fit",
        [RG_LANG_FR] = "Ajuster",
        [RG_LANG_DE] = "Einpassen",
    },
    {
        [RG_LANG_EN] = "Full ",
        [RG_LANG_FR] = "Remplir ",
        [RG_LANG_DE] = "Voll",
    },
    {
        [RG_LANG_EN] = "Zoom",
        [RG_LANG_FR] = "Zoomer",
        [RG_LANG_DE] = "Zoom",
    },

    // Led options
    {
        [RG_LANG_EN] = "LED options",
        [RG_LANG_FR] = "Options LED",
        [RG_LANG_DE] = "LED Optionen",
    },
    {
        [RG_LANG_EN] = "System activity",
        [RG_LANG_FR] = "Activité système",
        [RG_LANG_DE] = "Systemaktivität",
    },
    {
        [RG_LANG_EN] = "Disk activity",
        [RG_LANG_FR] = "Activité stockage",
        [RG_LANG_DE] = "SD-Aktivität",
    },
    {
        [RG_LANG_EN] = "Low battery",
        [RG_LANG_FR] = "Batterie basse",
        [RG_LANG_DE] = "Schwache Batterie",
    },
    {
        [RG_LANG_EN] = "Default",
        [RG_LANG_FR] = "Défaut",
        [RG_LANG_DE] = "Standard",
    },
    {
        [RG_LANG_EN] = "<None>",
        [RG_LANG_FR] = "<Aucun>",
        [RG_LANG_DE] = "<None>",
    },

    // Wifi
    {
        [RG_LANG_EN] = "Not connected",
        [RG_LANG_FR] = "Non connecté",
        [RG_LANG_DE] = "Nicht verbunden",
    },
    {
        [RG_LANG_EN] = "Connecting...",
        [RG_LANG_FR] = "Connexion...",
        [RG_LANG_DE] = "Verbinde...",
    },
    {
        [RG_LANG_EN] = "Disconnecting...",
        [RG_LANG_FR] = "Déconnexion...",
        [RG_LANG_DE] = "Trennen...",
    },
    {
        [RG_LANG_EN] = "(empty)",
        [RG_LANG_FR] = "(vide)",
        [RG_LANG_DE] = "(leer)",
    },
    {
        [RG_LANG_EN] = "Wi-Fi AP",
        [RG_LANG_FR] = "Wi-Fi AP",
        [RG_LANG_DE] = "Wi-Fi AP",
    },
    {
        [RG_LANG_EN] = "Start access point?\n\nSSID: retro-go\nPassword: retro-go\n\nBrowse: http://192.168.4.1/",
        [RG_LANG_FR] = "Démarrer point d'accès?\n\nSSID: retro-go\nPassword: retro-go\n\nAdresse: http://192.168.4.1/",
        [RG_LANG_DE] = "Starte Hotspot?\n\nSSID: retro-go\nPasswort: retro-go\n\nAdresse: http://192.168.4.1/",
    },
    {
        [RG_LANG_EN] = "Wi-Fi enable",
        [RG_LANG_FR] = "Activer Wi-Fi",
        [RG_LANG_DE] = "Wi-Fi aktivieren",
    },
    {
        [RG_LANG_EN] = "Wi-Fi access point",
        [RG_LANG_FR] = "Point d'accès WiFi",
        [RG_LANG_DE] = "Wi-Fi Hotspot",
    },
    {
        [RG_LANG_EN] = "Network",
        [RG_LANG_FR] = "Réseau",
        [RG_LANG_DE] = "Netzwerk",
    },
    {
        [RG_LANG_EN] = "IP address",
        [RG_LANG_FR] = "Adresse IP",
        [RG_LANG_DE] = "IP-Adresse",
    },

    // retro-go settings
    {
        [RG_LANG_EN] = "Brightness",
        [RG_LANG_FR] = "Luminosité",
        [RG_LANG_DE] = "Helligkeit",
    },
    {
        [RG_LANG_EN] = "Volume",
        [RG_LANG_FR] = "Volume",
        [RG_LANG_DE] = "Lautstärke",
    },
    {
        [RG_LANG_EN] = "Audio out",
        [RG_LANG_FR] = "Sortie audio",
        [RG_LANG_DE] = "Audioausgang",
    },
    {
        [RG_LANG_EN] = "Font type",
        [RG_LANG_FR] = "Police",
        [RG_LANG_DE] = "Schriftart",
    },
    {
        [RG_LANG_EN] = "Theme",
        [RG_LANG_FR] = "Thème",
        [RG_LANG_DE] = "Thema",
    },
    {
        [RG_LANG_EN] = "Show clock",
        [RG_LANG_FR] = "Horloge",
        [RG_LANG_DE] = "Uhr anzeigen",
    },
    {
        [RG_LANG_EN] = "Timezone",
        [RG_LANG_FR] = "Fuseau",
        [RG_LANG_DE] = "Zeitzone",
    },
    {
        [RG_LANG_EN] = "Wi-Fi options",
        [RG_LANG_FR] = "Options Wi-Fi",
        [RG_LANG_DE] = "Wi-Fi Optionen",
    },

    // app settings
    {
        [RG_LANG_EN] = "Scaling",
        [RG_LANG_FR] = "Format",
        [RG_LANG_DE] = "Skalierung",
    },
    {
        [RG_LANG_EN] = "Factor",
        [RG_LANG_FR] = "Factor",
        [RG_LANG_DE] = "Faktor",
    },
    {
        [RG_LANG_EN] = "Filter",
        [RG_LANG_FR] = "Filtre",
        [RG_LANG_DE] = "Filter",
    },
    {
        [RG_LANG_EN] = "Border",
        [RG_LANG_FR] = "Bordure",
        [RG_LANG_DE] = "Rand",
    },
    {
        [RG_LANG_EN] = "Speed",
        [RG_LANG_FR] = "Vitesse",
        [RG_LANG_DE] = "Geschwindigkeit",
    },

    // about menu
    {
        [RG_LANG_EN] = "Version",
        [RG_LANG_FR] = "Version",
        [RG_LANG_DE] = "Version",
    },
    {
        [RG_LANG_EN] = "Date",
        [RG_LANG_FR] = "Date",
        [RG_LANG_DE] = "Datum",
    },
    {
        [RG_LANG_EN] = "Target",
        [RG_LANG_FR] = "Appareil",
        [RG_LANG_DE] = "Ziel",
    },
    {
        [RG_LANG_EN] = "Website",
        [RG_LANG_FR] = "Site Web",
        [RG_LANG_DE] = "Webseite",
    },
    {
        [RG_LANG_EN] = "Options",
        [RG_LANG_FR] = "Options",
        [RG_LANG_DE] = "Optionen",
    },
    {
        [RG_LANG_EN] = "View credits",
        [RG_LANG_FR] = "Credits",
        [RG_LANG_DE] = "Credits",
    },
    {
        [RG_LANG_EN] = "Debug menu",
        [RG_LANG_FR] = "Menu debug",
        [RG_LANG_DE] = "Debug Menü",
    },
    {
        [RG_LANG_EN] = "Reset settings",
        [RG_LANG_FR] = "Reset paramètres",
        [RG_LANG_DE] = "Einstellungen zurücksetzen",
    },

    // save slot
    {
        [RG_LANG_EN] = "Slot 0",
        [RG_LANG_FR] = "Emplacement 0",
        [RG_LANG_DE] = "Speicherplatz 0",
    },
    {
        [RG_LANG_EN] = "Slot 1",
        [RG_LANG_FR] = "Emplacement 1",
        [RG_LANG_DE] = "Speicherplatz 1",
    },
    {
        [RG_LANG_EN] = "Slot 2",
        [RG_LANG_FR] = "Emplacement 2",
        [RG_LANG_DE] = "Speicherplatz 2",

    },
    {
        [RG_LANG_EN] = "Slot 3",
        [RG_LANG_FR] = "Emplacement 3",
        [RG_LANG_DE] = "Speicherplatz 3",
    },

    // game menu
    {
        [RG_LANG_EN] = "Save & Continue",
        [RG_LANG_FR] = "Sauver et continuer",
        [RG_LANG_DE] = "Speichern und weiter",
    },
    {
        [RG_LANG_EN] = "Save & Quit",
        [RG_LANG_FR] = "Sauver et quitter",
        [RG_LANG_DE] = "Speichern und Verlassen",
    },
    {
        [RG_LANG_EN] = "Load game",
        [RG_LANG_FR] = "Charger partie",
        [RG_LANG_DE] = "Spiel laden",
    },
    {
        [RG_LANG_EN] = "Reset",
        [RG_LANG_FR] = "Reset",
        [RG_LANG_DE] = "Reset",
    },
    {
        [RG_LANG_EN] = "Netplay",
        [RG_LANG_FR] = "Netplay",
        [RG_LANG_DE] = "Netplay",
    },
    {
        [RG_LANG_EN] = "About",
        [RG_LANG_FR] = "Infos",
        [RG_LANG_DE] = "Infos",
    },
    {
        [RG_LANG_EN] = "Quit",
        [RG_LANG_FR] = "Quitter",
        [RG_LANG_DE] = "Verlassen",
    },
    {
        [RG_LANG_EN] = "Soft reset",
        [RG_LANG_FR] = "Soft reset",
        [RG_LANG_DE] = "Soft reset",
    },
    {
        [RG_LANG_EN] = "Hard reset",
        [RG_LANG_FR] = "Hard reset",
        [RG_LANG_DE] = "Hard reset",
    },

    {
        [RG_LANG_EN] = "Reset Emulation?",
        [RG_LANG_FR] = "Reset Emulation?",
        [RG_LANG_DE] = "Emulation zurücksetzen?",
    },
    {
        [RG_LANG_EN] = "Save",
        [RG_LANG_FR] = "Sauver",
        [RG_LANG_DE] = "Speichern",
    },
    {
        [RG_LANG_EN] = "Load",
        [RG_LANG_FR] = "Charger",
        [RG_LANG_DE] = "Laden",
    },
    // end of rg_gui.c


    // main.c
    {
        [RG_LANG_EN] = "Show",
        [RG_LANG_FR] = "Montrer",
        [RG_LANG_DE] = "Zeigen",
    },
    {
        [RG_LANG_EN] = "Hide",
        [RG_LANG_FR] = "Cacher",
        [RG_LANG_DE] = "Verstecken",
    },
    {
        [RG_LANG_EN] = "Tabs Visibility",
        [RG_LANG_FR] = "Visibilité onglets",
        [RG_LANG_DE] = "Tabs sichtbar",
    },

    // scroll modes
    {
        [RG_LANG_EN] = "Center",
        [RG_LANG_FR] = "centrer",
        [RG_LANG_DE] = "Zentriert",
    },
    {
        [RG_LANG_EN] = "Paging",
        [RG_LANG_FR] = "Paging",
        [RG_LANG_DE] = "Blättern",
    },

    // start screen
    {
        [RG_LANG_EN] = "Auto",
        [RG_LANG_FR] = "Auto",
        [RG_LANG_DE] = "Auto",
    },
    {
        [RG_LANG_EN] = "Carousel",
        [RG_LANG_FR] = "Carousel",
        [RG_LANG_DE] = "Carousel",
    },
    {
        [RG_LANG_EN] = "Browser",
        [RG_LANG_FR] = "Browser",
        [RG_LANG_DE] = "Browser",
    },

    // preview
    {
        [RG_LANG_EN] = "None",
        [RG_LANG_FR] = "Aucun",
        [RG_LANG_DE] = "Nichts",
    },
    {
        [RG_LANG_EN] = "Cover,Save",
        [RG_LANG_FR] = "Cover,Save",
        [RG_LANG_DE] = "Cover, Speicherstand",
    },
    {
        [RG_LANG_EN] = "Save,Cover",
        [RG_LANG_FR] = "Save,Cover",
        [RG_LANG_DE] = "Speicherstand, Cover",
    },
    {
        [RG_LANG_EN] = "Cover only",
        [RG_LANG_FR] = "Cover only",
        [RG_LANG_DE] = "Nur Cover",
    },
    {
        [RG_LANG_EN] = "Save only",
        [RG_LANG_FR] = "Save only",
        [RG_LANG_DE] = "Nur Save",
    },

    // startup app
    {
        [RG_LANG_EN] = "Last game",
        [RG_LANG_FR] = "Dernier jeu",
        [RG_LANG_DE] = "Letztes Spiel",
    },
    {
        [RG_LANG_EN] = "Launcher",
        [RG_LANG_FR] = "Launcher",
        [RG_LANG_DE] = "Launcher",
    },

    // launcher options
    {
        [RG_LANG_EN] = "Launcher Options",
        [RG_LANG_FR] = "Options du lanceur",
        [RG_LANG_DE] = "Launcher Optionen",
    },
    {
        [RG_LANG_EN] = "Color theme",
        [RG_LANG_FR] = "Couleurs",
        [RG_LANG_DE] = "Farbthema",
    },
    {
        [RG_LANG_EN] = "Preview",
        [RG_LANG_FR] = "Aperçu",
        [RG_LANG_DE] = "Vorschau",
    },
    {
        [RG_LANG_EN] = "Scroll mode",
        [RG_LANG_FR] = "Mode défilement",
        [RG_LANG_DE] = "Scroll-Modus",
    },
    {
        [RG_LANG_EN] = "Start screen",
        [RG_LANG_FR] = "Écran démarrage",
        [RG_LANG_DE] = "Startbildschirm",
    },
    {
        [RG_LANG_EN] = "Hide tabs",
        [RG_LANG_FR] = "Cacher onglet",
        [RG_LANG_DE] = "Tabs verstecken",
    },
    {
        [RG_LANG_EN] = "File server",
        [RG_LANG_FR] = "Serveur fichier",
        [RG_LANG_DE] = "Datei Server",
    },
    {
        [RG_LANG_EN] = "Startup app",
        [RG_LANG_FR] = "App démarrage",
        [RG_LANG_DE] = "Start-App",
    },
    {
        [RG_LANG_EN] = "Build CRC cache",
        [RG_LANG_FR] = "Build CRC cache",
        [RG_LANG_DE] = "Build CRC cache",
    },
    {
        [RG_LANG_EN] = "Check for updates",
        [RG_LANG_FR] = "Verifier mise à jour",
        [RG_LANG_DE] = "Auf Updates prüfen",
    },
    {
        [RG_LANG_EN] = "HTTP Server Busy...",
        [RG_LANG_FR] = "Server Web ...",
        [RG_LANG_DE] = "HTTP Server Busy...",
    },
    {
        [RG_LANG_EN] = "SD Card Error",
        [RG_LANG_FR] = "Erreur carte SD",
        [RG_LANG_DE] = "SD-Card Fehler!",
    },
    {
        [RG_LANG_EN] = "Storage mount failed.\nMake sure the card is FAT32.",
        [RG_LANG_FR] = "Erreur carte SD.\nLa carte est bien en FAT32 ?",
        [RG_LANG_DE] = "Speicherbereitstellung fehlgeschlagen.\nStellen Sie sicher, dass die Karte FAT32 ist.",
    },
    // end of main.c


    // applications.c
    {
        [RG_LANG_EN] = "Scanning %s %d/%d",
        [RG_LANG_FR] = "Scan %s %d/%d",
        [RG_LANG_DE] = "Scannen %s %d/%d",
    },
    // message when no rom
    {
        [RG_LANG_EN] = "Welcome to Retro-Go!",
        [RG_LANG_FR] = "Bienvenue sur Retro-Go!",
        [RG_LANG_DE] = "Wilkommen bei Retro-Go!",
    },
    {
        [RG_LANG_EN] = "Place roms in folder: %s",
        [RG_LANG_FR] = "Placer les ROMS dans le dossier: %s",
        [RG_LANG_DE] = "Lege ROMs in den Ordner: %s",
    },
    {
        [RG_LANG_EN] = "With file extension: %s",
        [RG_LANG_FR] = "Avec l'extension: %s",
        [RG_LANG_DE] = "Mit Dateiendung: %s",
    },
    {
        [RG_LANG_EN] = "You can hide this tab in the menu",
        [RG_LANG_FR] = "Vous pouvez cacher cet onglet dans le menu",
        [RG_LANG_DE] = "Sie können diesen Reiter im Menü ausblenden",
    },
    {
        [RG_LANG_EN] = "You have no %s games",
        [RG_LANG_FR] = "Vous n'avez pas de jeux %s",
        [RG_LANG_DE] = "Du hast keine %s Spiele",
    },
    {
        [RG_LANG_EN] = "File not found",
        [RG_LANG_FR] = "Fichier non présent",
        [RG_LANG_DE] = "Datei nicht gefunden",
    },

    // rom options
    {
        [RG_LANG_EN] = "Name",
        [RG_LANG_FR] = "Nom",
        [RG_LANG_DE] = "Name",
    },
    {
        [RG_LANG_EN] = "Folder",
        [RG_LANG_FR] = "Dossier",
        [RG_LANG_DE] = "Ordner",
    },
    {
        [RG_LANG_EN] = "Size",
        [RG_LANG_FR] = "Taille",
        [RG_LANG_DE] = "Größe",
    },
    {
        [RG_LANG_EN] = "CRC32",
        [RG_LANG_FR] = "CRC32",
        [RG_LANG_DE] = "CRC32",
    },
    {
        [RG_LANG_EN] = "Delete file",
        [RG_LANG_FR] = "Supprimer fichier",
        [RG_LANG_DE] = "Datei löschen",
    },
    {
        [RG_LANG_EN] = "Close",
        [RG_LANG_FR] = "Fermer",
        [RG_LANG_DE] = "Schließen",
    },
    {
        [RG_LANG_EN] = "File properties",
        [RG_LANG_FR] = "Propriétés fichier",
        [RG_LANG_DE] = "Dateieigenschaften",
    },
    {
        [RG_LANG_EN] = "Delete selected file?",
        [RG_LANG_FR] = "Supprimer fichier?",
        [RG_LANG_DE] = "Ausgewählte Datei löschen?",
    },


    // in-game menu
    {
        [RG_LANG_EN] = "Resume game",
        [RG_LANG_FR] = "Reprendre partie",
        [RG_LANG_DE] = "Fortsetzen",
    },
    {
        [RG_LANG_EN] = "New game",
        [RG_LANG_FR] = "Nouvelle partie",
        [RG_LANG_DE] = "Neues Spiel",
    },
    {
        [RG_LANG_EN] = "Del favorite",
        [RG_LANG_FR] = "supp favori",
        [RG_LANG_DE] = "Favorit löschen",
    },
    {
        [RG_LANG_EN] = "Add favorite",
        [RG_LANG_FR] = "Ajouter favori",
        [RG_LANG_DE] = "Favorit hinzufügen",
    },
    {
        [RG_LANG_EN] = "Delete save",
        [RG_LANG_FR] = "Supp sauvegarde",
        [RG_LANG_DE] = "Speicherstand löschen?",
    },
    {
        [RG_LANG_EN] = "Properties",
        [RG_LANG_FR] = "Propriétés",
        [RG_LANG_DE] = "Eigenschaften",
    },
    {
        [RG_LANG_EN] = "Resume",
        [RG_LANG_FR] = "Reprendre",
        [RG_LANG_DE] = "Fortsetzen",
    },
    {
        [RG_LANG_EN] = "Delete save?",
        [RG_LANG_FR] = "Supp sauvegarde?",
        [RG_LANG_DE] = "Speicherstand löschen?",
    },
    {
        [RG_LANG_EN] = "Delete sram file?",
        [RG_LANG_FR] = "Supp fichier SRAM?",
        [RG_LANG_DE] = "SRAM-Datei löschen?",
    },
    // end of applications.c


    // rg_system.c
    {
        [RG_LANG_EN] = "App unresponsive... Hold MENU to quit!",
        [RG_LANG_FR] = "Plantage... MENU pour quitter",
        [RG_LANG_DE] = "App reagiert nicht... Halte MENU zum Beenden",
    },
    {
        [RG_LANG_EN] = "Reset all settings",
        [RG_LANG_FR] = "Reset paramètres",
        [RG_LANG_DE] = "Einstellungen zurücksetzen",
    },
    {
        [RG_LANG_EN] = "Reboot to factory ",
        [RG_LANG_FR] = "Redémarrer usine",
        [RG_LANG_DE] = "Neustart mit Werkseinstellungen",
    },
    {
        [RG_LANG_EN] = "Reboot to launcher",
        [RG_LANG_FR] = "Relancer launcher",
        [RG_LANG_DE] = "Neustart zum Launcher",
    },
    {
        [RG_LANG_EN] = "Recovery mode",
        [RG_LANG_FR] = "Mode de récupération",
        [RG_LANG_DE] = "Wiederherstellungsmodus",
    },
    {
        [RG_LANG_EN] = "System Panic!",
        [RG_LANG_FR] = "Plantage système!",
        [RG_LANG_DE] = "Systempanik!",
    },
    {
        [RG_LANG_EN] = "Save failed",
        [RG_LANG_FR] = "Erreur sauvegarde",
        [RG_LANG_DE] = "Speichern fehlgeschlagen",
    },
    // end of rg_system.c


    // store/main
    {
        [RG_LANG_EN] = "Store",
        [RG_LANG_FR] = "Boutique",
        [RG_LANG_DE] = "Store",
    },
    {
        [RG_LANG_EN] = "Store Menu",
        [RG_LANG_FR] = "Menu boutique",
        [RG_LANG_DE] = "Store-Menu",
    },
    {
        [RG_LANG_EN] = "Store Settings",
        [RG_LANG_FR] = "Parametres boutique",
        [RG_LANG_DE] = "Store-Einstellungen",
    },
    {
        [RG_LANG_EN] = "System Options",
        [RG_LANG_FR] = "Options systeme",
        [RG_LANG_DE] = "Systemoptionen",
    },
    {
        [RG_LANG_EN] = "Resume Browsing",
        [RG_LANG_FR] = "Reprendre",
        [RG_LANG_DE] = "Weiter browsen",
    },
    {
        [RG_LANG_EN] = "Exit to Launcher",
        [RG_LANG_FR] = "Quitter vers launcher",
        [RG_LANG_DE] = "Zum Launcher",
    },
    {
        [RG_LANG_EN] = "Game Menu",
        [RG_LANG_FR] = "Menu du jeu",
        [RG_LANG_DE] = "Spielmenu",
    },
    {
        [RG_LANG_EN] = "Install",
        [RG_LANG_FR] = "Installer",
        [RG_LANG_DE] = "Installieren",
    },
    {
        [RG_LANG_EN] = "Reinstall",
        [RG_LANG_FR] = "Reinstaller",
        [RG_LANG_DE] = "Neu installieren",
    },
    {
        [RG_LANG_EN] = "Play",
        [RG_LANG_FR] = "Jouer",
        [RG_LANG_DE] = "Spielen",
    },
    {
        [RG_LANG_EN] = "Play now?",
        [RG_LANG_FR] = "Jouer maintenant?",
        [RG_LANG_DE] = "Jetzt spielen?",
    },
    {
        [RG_LANG_EN] = "Download complete",
        [RG_LANG_FR] = "Telechargement termine",
        [RG_LANG_DE] = "Download fertig",
    },
    {
        [RG_LANG_EN] = "Download failed",
        [RG_LANG_FR] = "Echec du telechargement",
        [RG_LANG_DE] = "Download fehlgeschlagen",
    },
    {
        [RG_LANG_EN] = "Downloading...",
        [RG_LANG_FR] = "Telechargement...",
        [RG_LANG_DE] = "Herunterladen...",
    },
    {
        [RG_LANG_EN] = "Loading catalog...",
        [RG_LANG_FR] = "Chargement du catalogue...",
        [RG_LANG_DE] = "Katalog wird geladen...",
    },
    {
        [RG_LANG_EN] = "Connecting...",
        [RG_LANG_FR] = "Connexion...",
        [RG_LANG_DE] = "Verbinden...",
    },
    {
        [RG_LANG_EN] = "Downloading cover art...",
        [RG_LANG_FR] = "Telechargement jaquette...",
        [RG_LANG_DE] = "Cover wird geladen...",
    },
    {
        [RG_LANG_EN] = "Cover Art",
        [RG_LANG_FR] = "Jaquette",
        [RG_LANG_DE] = "Cover",
    },
    {
        [RG_LANG_EN] = "Cover art",
        [RG_LANG_FR] = "Jaquette",
        [RG_LANG_DE] = "Cover",
    },
    {
        [RG_LANG_EN] = "Fetch cover art",
        [RG_LANG_FR] = "Telecharger la jaquette",
        [RG_LANG_DE] = "Cover laden",
    },
    {
        [RG_LANG_EN] = "Info",
        [RG_LANG_FR] = "Infos",
        [RG_LANG_DE] = "Info",
    },
    {
        [RG_LANG_EN] = "Game Info",
        [RG_LANG_FR] = "Infos du jeu",
        [RG_LANG_DE] = "Spielinfo",
    },
    {
        [RG_LANG_EN] = "Delete from SD",
        [RG_LANG_FR] = "Supprimer de la SD",
        [RG_LANG_DE] = "Von SD loschen",
    },
    {
        [RG_LANG_EN] = "Delete Game",
        [RG_LANG_FR] = "Supprimer le jeu",
        [RG_LANG_DE] = "Spiel loschen",
    },
    {
        [RG_LANG_EN] = "Deleted",
        [RG_LANG_FR] = "Supprime",
        [RG_LANG_DE] = "Geloscht",
    },
    {
        [RG_LANG_EN] = "Search / Filter",
        [RG_LANG_FR] = "Rechercher / Filtrer",
        [RG_LANG_DE] = "Suchen / Filtern",
    },
    {
        [RG_LANG_EN] = "Search Games",
        [RG_LANG_FR] = "Rechercher un jeu",
        [RG_LANG_DE] = "Spiele suchen",
    },
    {
        [RG_LANG_EN] = "Clear filter",
        [RG_LANG_FR] = "Effacer le filtre",
        [RG_LANG_DE] = "Filter loschen",
    },
    {
        [RG_LANG_EN] = "Enter part of a file name:",
        [RG_LANG_FR] = "Entrez une partie du nom:",
        [RG_LANG_DE] = "Teil des Dateinamens:",
    },
    {
        [RG_LANG_EN] = "Sort",
        [RG_LANG_FR] = "Tri",
        [RG_LANG_DE] = "Sortierung",
    },
    {
        [RG_LANG_EN] = "Sort Games",
        [RG_LANG_FR] = "Trier les jeux",
        [RG_LANG_DE] = "Spiele sortieren",
    },
    {
        [RG_LANG_EN] = "Newest",
        [RG_LANG_FR] = "Recents",
        [RG_LANG_DE] = "Neueste",
    },
    {
        [RG_LANG_EN] = "Downloaded",
        [RG_LANG_FR] = "Telecharges",
        [RG_LANG_DE] = "Heruntergeladen",
    },
    {
        [RG_LANG_EN] = "Hide unavailable systems",
        [RG_LANG_FR] = "Masquer systemes indisponibles",
        [RG_LANG_DE] = "Fehlende Systeme ausblenden",
    },
    {
        [RG_LANG_EN] = "Store URL",
        [RG_LANG_FR] = "URL boutique",
        [RG_LANG_DE] = "Store-URL",
    },
    {
        [RG_LANG_EN] = "Default sort",
        [RG_LANG_FR] = "Tri par defaut",
        [RG_LANG_DE] = "Standard-Sortierung",
    },
    {
        [RG_LANG_EN] = "Test connection",
        [RG_LANG_FR] = "Tester connexion",
        [RG_LANG_DE] = "Verbindung testen",
    },
    {
        [RG_LANG_EN] = "Test Connection",
        [RG_LANG_FR] = "Test connexion",
        [RG_LANG_DE] = "Verbindungstest",
    },
    {
        [RG_LANG_EN] = "Back",
        [RG_LANG_FR] = "Retour",
        [RG_LANG_DE] = "Zuruck",
    },
    {
        [RG_LANG_EN] = "Filter",
        [RG_LANG_FR] = "Filtre",
        [RG_LANG_DE] = "Filter",
    },
    {
        [RG_LANG_EN] = "No matches.",
        [RG_LANG_FR] = "Aucun resultat.",
        [RG_LANG_DE] = "Keine Treffer.",
    },
    {
        [RG_LANG_EN] = "No Games",
        [RG_LANG_FR] = "Aucun jeu",
        [RG_LANG_DE] = "Keine Spiele",
    },
    {
        [RG_LANG_EN] = "No available systems.",
        [RG_LANG_FR] = "Aucun systeme disponible.",
        [RG_LANG_DE] = "Keine Systeme verfugbar.",
    },
    {
        [RG_LANG_EN] = "Not installed",
        [RG_LANG_FR] = "Non installe",
        [RG_LANG_DE] = "Nicht installiert",
    },
    {
        [RG_LANG_EN] = "Not set",
        [RG_LANG_FR] = "Non defini",
        [RG_LANG_DE] = "Nicht gesetzt",
    },
    {
        [RG_LANG_EN] = "Unknown",
        [RG_LANG_FR] = "Inconnu",
        [RG_LANG_DE] = "Unbekannt",
    },
    {
        [RG_LANG_EN] = "Unknown size",
        [RG_LANG_FR] = "Taille inconnue",
        [RG_LANG_DE] = "Unbekannte Große",
    },
    {
        [RG_LANG_EN] = "Size",
        [RG_LANG_FR] = "Taille",
        [RG_LANG_DE] = "Große",
    },
    {
        [RG_LANG_EN] = "Remote size",
        [RG_LANG_FR] = "Taille distante",
        [RG_LANG_DE] = "Online-Große",
    },
    {
        [RG_LANG_EN] = "File",
        [RG_LANG_FR] = "Fichier",
        [RG_LANG_DE] = "Datei",
    },
    {
        [RG_LANG_EN] = "Path",
        [RG_LANG_FR] = "Chemin",
        [RG_LANG_DE] = "Pfad",
    },
    {
        [RG_LANG_EN] = "System",
        [RG_LANG_FR] = "Systeme",
        [RG_LANG_DE] = "System",
    },
    {
        [RG_LANG_EN] = "Included",
        [RG_LANG_FR] = "Inclus",
        [RG_LANG_DE] = "Enthalten",
    },
    {
        [RG_LANG_EN] = "Cannot play",
        [RG_LANG_FR] = "Impossible de jouer",
        [RG_LANG_DE] = "Spielen nicht moglich",
    },
    {
        [RG_LANG_EN] = "Install the game first.",
        [RG_LANG_FR] = "Installez d'abord le jeu.",
        [RG_LANG_DE] = "Spiel zuerst installieren.",
    },
    {
        [RG_LANG_EN] = "Game saved to SD Card.",
        [RG_LANG_FR] = "Jeu enregistre sur la SD.",
        [RG_LANG_DE] = "Spiel auf SD gespeichert.",
    },
    {
        [RG_LANG_EN] = "Cover art saved.",
        [RG_LANG_FR] = "Jaquette enregistree.",
        [RG_LANG_DE] = "Cover gespeichert.",
    },
    {
        [RG_LANG_EN] = "Success!",
        [RG_LANG_FR] = "Succes!",
        [RG_LANG_DE] = "Erfolg!",
    },
    {
        [RG_LANG_EN] = "B: Cancel",
        [RG_LANG_FR] = "B: Annuler",
        [RG_LANG_DE] = "B: Abbrechen",
    },
    {
        [RG_LANG_EN] = "MENU: Menu",
        [RG_LANG_FR] = "MENU: Menu",
        [RG_LANG_DE] = "MENU: Menu",
    },
    {
        [RG_LANG_EN] = "A: Install   B: Back   MENU: Menu",
        [RG_LANG_FR] = "A: Installer   B: Retour   MENU: Menu",
        [RG_LANG_DE] = "A: Installieren   B: Zuruck   MENU: Menu",
    },
    {
        [RG_LANG_EN] = "A: Open   B: Back   MENU: Menu",
        [RG_LANG_FR] = "A: Ouvrir   B: Retour   MENU: Menu",
        [RG_LANG_DE] = "A: Offnen   B: Zuruck   MENU: Menu",
    },
    {
        [RG_LANG_EN] = "Network Error",
        [RG_LANG_FR] = "Erreur reseau",
        [RG_LANG_DE] = "Netzwerkfehler",
    },
    {
        [RG_LANG_EN] = "Wi-Fi is not connected.",
        [RG_LANG_FR] = "Wi-Fi non connecte.",
        [RG_LANG_DE] = "Wi-Fi ist nicht verbunden.",
    },
    {
        [RG_LANG_EN] = "Reconnecting Wi-Fi...",
        [RG_LANG_FR] = "Reconnexion Wi-Fi...",
        [RG_LANG_DE] = "Wi-Fi neu verbinden...",
    },
    {
        [RG_LANG_EN] = "Starting Wi-Fi...",
        [RG_LANG_FR] = "Demarrage Wi-Fi...",
        [RG_LANG_DE] = "Wi-Fi startet...",
    },
    {
        [RG_LANG_EN] = "Could not connect to Wi-Fi.",
        [RG_LANG_FR] = "Connexion Wi-Fi impossible.",
        [RG_LANG_DE] = "Wi-Fi-Verbindung fehlgeschlagen.",
    },
    {
        [RG_LANG_EN] = "Invalid store URL",
        [RG_LANG_FR] = "URL boutique invalide",
        [RG_LANG_DE] = "Ungultige Store-URL",
    },
    {
        [RG_LANG_EN] = "The store URL is too long.",
        [RG_LANG_FR] = "L'URL de la boutique est trop longue.",
        [RG_LANG_DE] = "Die Store-URL ist zu lang.",
    },
    {
        [RG_LANG_EN] = "Store not configured",
        [RG_LANG_FR] = "Boutique non configuree",
        [RG_LANG_DE] = "Store nicht eingerichtet",
    },
    {
        [RG_LANG_EN] = "Store URL saved.",
        [RG_LANG_FR] = "URL boutique enregistree.",
        [RG_LANG_DE] = "Store-URL gespeichert.",
    },
    {
        [RG_LANG_EN] = "Store returned HTTP %d.",
        [RG_LANG_FR] = "Boutique: HTTP %d.",
        [RG_LANG_DE] = "Store meldet HTTP %d.",
    },
    {
        [RG_LANG_EN] = "Connection OK (HTTP %d).",
        [RG_LANG_FR] = "Connexion OK (HTTP %d).",
        [RG_LANG_DE] = "Verbindung OK (HTTP %d).",
    },
    {
        [RG_LANG_EN] = "Connection failed (HTTP %d).",
        [RG_LANG_FR] = "Connexion echouee (HTTP %d).",
        [RG_LANG_DE] = "Verbindung fehlgeschlagen (HTTP %d).",
    },
    {
        [RG_LANG_EN] = "Could not reach store.",
        [RG_LANG_FR] = "Boutique inaccessible.",
        [RG_LANG_DE] = "Store nicht erreichbar.",
    },
    {
        [RG_LANG_EN] = "No %s catalog found (HTTP 404).",
        [RG_LANG_FR] = "Aucun catalogue %s trouve (HTTP 404).",
        [RG_LANG_DE] = "Kein %s-Katalog gefunden (HTTP 404).",
    },
    {
        [RG_LANG_EN] = "Server reached, but no catalog was found (HTTP 404).",
        [RG_LANG_FR] = "Serveur joignable, mais aucun catalogue trouve (HTTP 404).",
        [RG_LANG_DE] = "Server erreichbar, aber kein Katalog gefunden (HTTP 404).",
    },
    {
        [RG_LANG_EN] = "Could not read the store catalog.",
        [RG_LANG_FR] = "Lecture du catalogue impossible.",
        [RG_LANG_DE] = "Store-Katalog nicht lesbar.",
    },
    {
        [RG_LANG_EN] = "The store catalog is not valid JSON.",
        [RG_LANG_FR] = "Catalogue JSON invalide.",
        [RG_LANG_DE] = "Store-Katalog ist kein gultiges JSON.",
    },
    {
        [RG_LANG_EN] = "The store has no games for this system.",
        [RG_LANG_FR] = "Aucun jeu pour ce systeme.",
        [RG_LANG_DE] = "Keine Spiele fur dieses System.",
    },
    {
        [RG_LANG_EN] = "Out of Memory",
        [RG_LANG_FR] = "Memoire insuffisante",
        [RG_LANG_DE] = "Zu wenig Speicher",
    },
    {
        [RG_LANG_EN] = "Could not create HTTP client.",
        [RG_LANG_FR] = "Client HTTP impossible.",
        [RG_LANG_DE] = "HTTP-Client nicht moglich.",
    },
    {
        [RG_LANG_EN] = "Not enough free space on the SD card.",
        [RG_LANG_FR] = "Espace insuffisant sur la SD.",
        [RG_LANG_DE] = "Nicht genug Platz auf der SD-Karte.",
    },
    {
        [RG_LANG_EN] = "Could not write to the SD card.",
        [RG_LANG_FR] = "Ecriture SD impossible.",
        [RG_LANG_DE] = "Schreiben auf SD fehlgeschlagen.",
    },
    {
        [RG_LANG_EN] = "The store returned an empty file.",
        [RG_LANG_FR] = "Fichier vide recu.",
        [RG_LANG_DE] = "Store lieferte eine leere Datei.",
    },
    {
        [RG_LANG_EN] = "Download cancelled.",
        [RG_LANG_FR] = "Telechargement annule.",
        [RG_LANG_DE] = "Download abgebrochen.",
    },
    {
        [RG_LANG_EN] = "This system is not installed.\nInstall its app first.",
        [RG_LANG_FR] = "Systeme non installe.\nInstallez son app d'abord.",
        [RG_LANG_DE] = "System nicht installiert.\nZuerst die App installieren.",
    },
    {
        [RG_LANG_EN] = "PICO-8 carts include their own art.",
        [RG_LANG_FR] = "Les carts PICO-8 ont deja leur image.",
        [RG_LANG_DE] = "PICO-8-Carts enthalten ihr eigenes Bild.",
    },
    {
        [RG_LANG_EN] = "Replace the existing cover art?",
        [RG_LANG_FR] = "Remplacer la jaquette existante?",
        [RG_LANG_DE] = "Vorhandenes Cover ersetzen?",
    },
    {
        [RG_LANG_EN] = "Cover art is already present.\nDownload a new one?",
        [RG_LANG_FR] = "Jaquette deja presente.\nEn telecharger une nouvelle?",
        [RG_LANG_DE] = "Cover ist vorhanden.\nNeues herunterladen?",
    },
    {
        [RG_LANG_EN] = "Download cover art for this game?",
        [RG_LANG_FR] = "Telecharger la jaquette?",
        [RG_LANG_DE] = "Cover fur dieses Spiel laden?",
    },
    {
        [RG_LANG_EN] = "Reinstall game?",
        [RG_LANG_FR] = "Reinstaller le jeu?",
        [RG_LANG_DE] = "Spiel neu installieren?",
    },
    {
        [RG_LANG_EN] = "Replace the existing file?",
        [RG_LANG_FR] = "Remplacer le fichier existant?",
        [RG_LANG_DE] = "Vorhandene Datei ersetzen?",
    },
    {
        [RG_LANG_EN] = "Delete this game and its cover art?\n%s",
        [RG_LANG_FR] = "Supprimer le jeu et sa jaquette?\n%s",
        [RG_LANG_DE] = "Spiel und Cover loschen?\n%s",
    },
    {
        [RG_LANG_EN] = "Delete failed",
        [RG_LANG_FR] = "Echec suppression",
        [RG_LANG_DE] = "Loschen fehlgeschlagen",
    },
    {
        [RG_LANG_EN] = "Could not delete the file.",
        [RG_LANG_FR] = "Suppression impossible.",
        [RG_LANG_DE] = "Datei konnte nicht geloscht werden.",
    },
    {
        [RG_LANG_EN] = "Game removed from the SD card.",
        [RG_LANG_FR] = "Jeu supprime de la SD.",
        [RG_LANG_DE] = "Spiel von der SD-Karte entfernt.",
    },
    {
        [RG_LANG_EN] = "No cover art for this game\nin your store yet.\nAdd it with the ROM Art\nManager page.",
        [RG_LANG_FR] = "Pas encore de jaquette\ndans votre boutique.\nAjoutez-la avec la page\nROM Art Manager.",
        [RG_LANG_DE] = "Noch kein Cover fur dieses\nSpiel im Store. Fuge es mit\nder ROM Art Manager Seite\nhinzu.",
    },
    {
        [RG_LANG_EN] = "Network error while\ndownloading cover art.",
        [RG_LANG_FR] = "Erreur reseau pendant\nle telechargement.",
        [RG_LANG_DE] = "Netzwerkfehler beim\nCover-Download.",
    },
    {
        [RG_LANG_EN] = "Cover image is too large.",
        [RG_LANG_FR] = "Image de jaquette trop grande.",
        [RG_LANG_DE] = "Cover-Bild ist zu gross.",
    },
    {
        [RG_LANG_EN] = "Downloaded file is not\na valid PNG image.",
        [RG_LANG_FR] = "Le fichier n'est pas\nune image PNG valide.",
        [RG_LANG_DE] = "Datei ist kein\ngultiges PNG-Bild.",
    },
    {
        [RG_LANG_EN] = "Could not save the cover\nto the SD card.",
        [RG_LANG_FR] = "Impossible d'enregistrer\nla jaquette sur la SD.",
        [RG_LANG_DE] = "Cover konnte nicht auf\ndie SD-Karte gespeichert werden.",
    },
    {
        [RG_LANG_EN] = "Unknown error.",
        [RG_LANG_FR] = "Erreur inconnue.",
        [RG_LANG_DE] = "Unbekannter Fehler.",
    },
    {
        [RG_LANG_EN] = "Unknown size",
        [RG_LANG_FR] = "Taille inconnue",
        [RG_LANG_DE] = "Unbekannte Große",
    },
    {
        [RG_LANG_EN] = "Set a valid store URL first.",
        [RG_LANG_FR] = "Definissez d'abord une URL valide.",
        [RG_LANG_DE] = "Zuerst eine gultige Store-URL setzen.",
    },
    {
        [RG_LANG_EN] = "The URL must start with http:// or https://\nand must not contain the placeholder.",
        [RG_LANG_FR] = "L'URL doit commencer par http:// ou https://\net ne pas contenir le texte d'exemple.",
        [RG_LANG_DE] = "Die URL muss mit http:// oder https:// beginnen\nund darf keinen Platzhalter enthalten.",
    },
};
