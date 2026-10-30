#pragma once
// Lang.h - UI strings in English and Hungarian.
// To add a new language: add a value to AppLang, a new row to g_strings,
// and a menu entry in BuildMenu() (Commander.cpp).

enum AppLang { APPLANG_EN = 0, APPLANG_HU = 1 };

enum Str {
    S_APP_TITLE,
    S_MENU_FILES, S_MENU_BACKUP, S_MENU_TOOLS, S_MENU_LANG, S_MENU_HELP,
    S_RENAME, S_EDIT, S_COPY, S_MOVE, S_NEWFOLDER, S_DELETE, S_BACKUPCOPY, S_EXIT,
    S_REFRESH, S_SAVETREE, S_ABOUT, S_ABOUT_TEXT,
    S_COL_NAME, S_COL_EXT, S_COL_SIZE, S_COL_DATE,
    S_NOTHING_SELECTED,
    S_CONFIRM_COPY, S_CONFIRM_MOVE, S_CONFIRM_BACKUP,
    S_SAME_FOLDER, S_INTO_ITSELF,
    S_ERR_OPEN_FOLDER, S_ERR_RENAME, S_ERR_INVALID_NAME,
    S_ERR_CREATE_BACKUP, S_ERR_CREATE_FOLDER,
    S_BACKUP_DONE, S_INFO, S_NEW_FOLDER_NAME,
    S_ERR_DEVICE_UNSUPPORTED, S_ERR_DEVICE_EMPTY,
    S_COUNT
};

static const wchar_t* const g_strings[2][S_COUNT] = {
    // ---------------------------------------------------------------- English
    {
        L"Vault Commander",
        L"&Files", L"&Backup", L"&Tools", L"&Language", L"&Help",
        L"Rename", L"Edit", L"Copy", L"Move", L"New folder", L"Delete", L"Backup copy", L"Exit",
        L"Refresh", L"Save directory tree to text file...", L"About",
        L"Two-pane file manager with time-stamped backup copy.\n"
        L"Backup copies always go into a new folder named after the date and time; "
        L"earlier backups are never overwritten.",
        L"Name", L"Ext", L"Size", L"Modified",
        L"No files or folders selected.",
        L"Copy %d item(s) to:\n%s",
        L"Move %d item(s) to:\n%s",
        L"Create a backup copy of %d item(s)?\n\n"
        L"A new folder named after the current date and time will be created in:\n%s",
        L"Source and destination folders are the same.",
        L"A folder cannot be copied into itself (or into one of its subfolders).",
        L"Cannot open folder:\n%s",
        L"Rename failed (error code: %lu).",
        L"Invalid name. These characters are not allowed: \\ / : * ? \" < > |",
        L"Cannot create the backup folder (error code: %lu).",
        L"Cannot create the folder (error code: %lu).",
        L"Backup created: %s",
        L"%s / %s - %d / %d file(s), %d / %d folder(s)",
        L"New folder",
        L"This operation is not available on a mobile device. You can browse it, copy, move and delete files, "
        L"and make backup copies from the phone into a folder on the PC.",
        L"No storage found on the device. Unlock the phone and set the USB mode to \"File transfer\" (MTP), "
        L"then open the device again."
    },
    // -------------------------------------------------------------- Hungarian
    {
        L"Vault Commander",
        L"&Fájl", L"&Biztonsági mentés", L"&Eszközök", L"&Nyelv", L"&Súgó",
        L"Átnevezés", L"Szerkesztés", L"Másolás", L"Áthelyezés", L"Új mappa", L"Törlés", L"Biztonsági másolás", L"Kilépés",
        L"Frissítés", L"Mappaszerkezet mentése szövegfájlba...", L"Névjegy",
        L"Kétpaneles fájlkezelő időbélyeges biztonsági másolással.\n"
        L"A biztonsági másolatok mindig új, a dátumról és időről elnevezett mappába kerülnek; "
        L"a korábbi mentéseket a program soha nem írja felül.",
        L"Név", L"Kit.", L"Méret", L"Módosítva",
        L"Nincs kijelölt fájl vagy mappa.",
        L"%d elem másolása ide:\n%s",
        L"%d elem áthelyezése ide:\n%s",
        L"Biztonsági másolat készítése %d elemről?\n\n"
        L"Ebben a mappában új, a dátumról és időről elnevezett mappa jön létre:\n%s",
        L"A forrás és a cél mappa megegyezik.",
        L"Egy mappa nem másolható saját magába (vagy annak almappájába).",
        L"A mappa nem nyitható meg:\n%s",
        L"Az átnevezés nem sikerült (hibakód: %lu).",
        L"Érvénytelen név. Ezek a karakterek nem engedélyezettek: \\ / : * ? \" < > |",
        L"A biztonsági mentés mappája nem hozható létre (hibakód: %lu).",
        L"A mappa nem hozható létre (hibakód: %lu).",
        L"Biztonsági mentés létrehozva: %s",
        L"%s / %s - %d / %d fájl, %d / %d mappa",
        L"Új mappa",
        L"Ez a művelet mobileszközön nem érhető el. Böngészhető, a fájlok másolhatók, áthelyezhetők és törölhetők, "
        L"és a telefonról biztonsági másolat készíthető a számítógép egyik mappájába.",
        L"Az eszközön nem található tárhely. Oldd fel a telefont, és állítsd az USB módot „Fájlátvitel” (MTP) értékre, "
        L"majd nyisd meg újra az eszközt."
    }
};

static AppLang g_lang = APPLANG_EN;

inline const wchar_t* T(Str id) { return g_strings[g_lang][id]; }
