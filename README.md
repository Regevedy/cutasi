# Cao Urban Trainer ASI v1.0

**Author:** CaoK1d13  
**Platform:** GTA V Story Mode / Offline  
**Format:** native ScriptHookV `.asi` source project

Cao Urban Trainer ASI v1.0 is a full offline sandbox trainer for GTA V Story Mode. It includes player tools, weapons, vehicles, NPC control, bodyguards, garage, battle arena, mission/scenario creator, object placement, cinematic tools, world controls, teleport tools, presets, and multi-language support.

## Important

This project is for **GTA V Story Mode / Offline only**. Do not use it in GTA Online.

This archive contains source code. Build it with **Visual Studio 2022 x64** and the **ScriptHookV SDK** to produce `CaoUrbanTrainer.asi`.

## Main controls

```text
F5        Open / close trainer
↑ / ↓     Navigate
← / →     Change value
Enter     Apply / open
Backspace Back
Esc       Close menu
F7        Toggle Mission Creator free camera
```

## Included systems

- Player Menu
- Advanced Player Menu
- Skin / Model Changer
- Outfit / Clothes Editor
- Weapons Menu
- Advanced Weapon Menu
- Vehicle Menu
- Vehicle Spawner
- Vehicle Builder
- Vehicle Tuning
- Garage System
- NPC Menu
- NPC Model Browser
- Chauffeur / Driver Mode
- Bodyguards Menu
- Enemy Spawner / Battle Arena
- Scenario Menu
- Mission / Scenario Creator
- Object Spawner
- Animation Menu
- Cinematic Free Camera
- Screenshot / Cinematic Tools
- World Control Menu
- Advanced Teleport Menu
- Police / Wanted Menu
- Save / Load Trainer Presets
- Settings Menu
- Language Selector

## Mission / Scenario Creator

The mission creator lets you build custom scenes and missions inside the game. You can place NPCs, vehicles, objects, mission markers, objective blips and routes, then playtest the result.

During mission creation, the trainer can automatically clear nearby ambient NPCs and vehicles so they do not block your scene. This can be disabled in Settings or in `CaoUrbanTrainerASI.ini`.

```ini
[MissionCreator]
MissionCreatorFreeCamKey=F7
MissionCreatorAutoClearWorld=true
MissionCreatorCleanRadius=70.0
```

## Languages

The Settings menu supports:

```text
English
Russian
Chinese
French
German
Japanese
Spanish
Italian
```

Default language:

```ini
[Language]
Language=English
```

## Required files after build

Place these files near `GTA5.exe` after building the `.asi`:

```text
Grand Theft Auto V/
├── GTA5.exe
├── ScriptHookV.dll
├── dinput8.dll
├── CaoUrbanTrainer.asi
├── CaoUrbanTrainerASI.ini
├── CaoUrbanTrainer_PedModels.txt
├── CaoUrbanTrainer_Vehicles.txt
├── CaoUrbanTrainer_AddonVehicles.txt
├── CaoUrbanTrainer_Garage.txt
├── CaoUrbanTrainer_Outfits.txt
├── CaoUrbanTrainer_Objects.txt
├── CaoUrbanTrainer_Locations.txt
├── CaoUrbanTrainer_Missions.txt
└── CaoUrbanTrainer_Presets.txt
```

## Build instructions

1. Download the ScriptHookV SDK.
2. Put SDK headers and libraries into:

```text
vendor/ScriptHookV_SDK/inc/
vendor/ScriptHookV_SDK/lib/
```

3. Open:

```text
CaoUrbanTrainerASI.sln
```

4. Select:

```text
Release | x64
```

5. Build the project.

Expected output:

```text
bin/Release/CaoUrbanTrainer.asi
```

## Notes

Some GTA V models have limited AI support. Animals, cutscene-only models, and special story peds may not drive, shoot, wear clothes, or play all animations correctly. That is a GTA V engine limitation.
