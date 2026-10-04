# Arbeitsregeln für diesen Fork (ReSkate Hall of Meat & Freunde)

## ReSkate-first: Vorhandenes erweitern, nicht duplizieren

Bevor wir eigene Native-Zugriffe bauen, IMMER im ReSkate-Quellcode prüfen, ob die
Infrastruktur schon existiert — sie ist meist reverse-engineered, verifiziert und
in Produktion:

- **Skeleton/Pose**: `Extension/Multiplayer/Remote/native_pose_layout.h` (Pose-Layout,
  395-Joint-Skeleton) + `Extension/Skater/client_first_person.cpp` (Joint-Composition
  in Weltkoordinaten). Statt das zu kopieren: gemeinsame API daraus extrahieren und
  beide Nutzer darauf fahren.
- **Hooks**: Ein Detours-Hook pro Zieladresse gehört einem Feature (siehe
  `Extension/Skater/no_bail.cpp`). Neue Features, die dieselbe Adresse brauchen,
  koppeln an den bestehenden Hook (Beobachter-Muster), nicht zweiter Hook.
- **Overlay/Rendering**: Welt→Screen-Projektion und Kamera-Matrix sind in
  `Extension/UI/Overlay/` (Nametags, Park-Editor) bereits gelöst — wiederverwenden.
- **RE-Fundstellen**: Neue Native-Layouts werden im Stil von
  `Engine/Game/Build/20260929/no_bail.h` dokumentiert (Adresse, Offsets, Vertrag,
  Messdatum), nicht in Kommentaren vergraben.

## Grundhaltung

- Wir erweitern das **gesamte ReSkate-Projekt** (API vergrößern, Code teilen),
  nicht nur unseren eigenen Mod. Ein Feature, das nur für sich selbst kopiert,
  ist ein Bug.
- Eigener State bleibt klein, sandboxed, noexcept; Spieldaten nur über geprüfte
  Ownership-Ketten lesen (`resolve()`-Muster), `__try`/LastError in Hooks.
- `/W4 /WX` ist Standard; Commits im Repo-Stil (Imperativ, ausführliche Begründung).
- Tests nach Repo-Muster (Test/Ordner, Fake-Clock/Include-Trick wie
  `camera_observer_tests`).
