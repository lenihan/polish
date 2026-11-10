#pragma once

// Shared between app.rc and TrayIcon.cpp so the resource ID used to embed
// the icon and the ID used to load it via LoadIconW stay in sync.
#define IDI_POLISH_TRAY 101
// The icon a stack's taskbar button shows (StackStripWindow).
#define IDI_POLISH_STACK 102
// The same icon with a dark outline, for a light taskbar.
#define IDI_POLISH_STACK_LIGHT 103
// The Polish icon with a dark outline and title bar, for a light taskbar.
#define IDI_POLISH_TRAY_LIGHT 104
