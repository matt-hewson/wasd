// src/ds3/poll.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// One attached game run: polls until the game exits or the guide stops
// (WindowPollWorker below waits for the game and calls this per run).
void PollGameRun(HANDLE hProcess, DWORD pid) {
  WindowSnapshot snap;
  snap.pid = pid;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (!moduleBase) {  // module list not ready yet (game still starting): retry later
    CloseHandle(hProcess);
    return;
  }
  EnterCriticalSection(&g_trail.lock);  // the map shows this run's path only
  g_trail.points.clear();
  LeaveCriticalSection(&g_trail.lock);
  const uintptr_t gameManCellOffset =
      moduleBase ? ResolveGameManCellOffset(hProcess, moduleBase, moduleSize) : 0;
  const AreaNames areaNames;
  AreaTracker areaTracker;
  const AreaInfo areaInfo;
  SessionTracker tracker;
  tracker.log.enabled = true;  // window/overlay sessions are always saved
  SessionStats& session = tracker.stats;
  LARGE_INTEGER lastSessionTick{};

  // Everything below is (re)built by resolveAll() whenever the game's
  // current player object changes -- see ReadCurrentPlayerIns.
  ResolvedPointers resolved;
  ResolvedProfile profile;
  uintptr_t wcmCellOffset = 0;
  uintptr_t equipInventoryData = 0;
  std::vector<SlotInfo> slots;
  std::vector<int32_t> lastSlotIds;
  int32_t lastLevel = INT32_MIN;
  int32_t lastHandIdx[3] = {INT32_MIN, INT32_MIN, INT32_MIN};  // right, left, quick item
  int32_t lastSpellIdx = INT32_MIN;
  std::vector<SessionBoss> sessionBosses;  // every boss's defeat flag, from LiveBossList (for the session log)
  SessionWeapon handWeapons[2];            // the weapon in each hand (for the session log)
  // Gear is re-resolved when a slot's inventory entry changes. An upgrade
  // keeps the entry (only the weapon's id changes) and picking up materials
  // changes no slot, so those are checked apart (each second / every 5s)
  // and set refreshGear for the next tick. Only the materials the equipped
  // weapons' next upgrades need count (not Estus or other goods).
  int32_t lastWeaponIds[6] = {INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN};
  std::unordered_map<int32_t, int> upgradeMaterials;  // goods id -> held, as of the last refresh
  bool refreshGear = false;
  bool inGame = false;
  ULONGLONG lastResolveAttempt = 0;
  auto resolveAll = [&]() -> bool {
    resolved = moduleBase ? ResolvePlayerHp(hProcess, moduleBase, moduleSize) : ResolvedPointers{};
    if (resolved.worldChrManCellOffset) wcmCellOffset = resolved.worldChrManCellOffset;
    profile = moduleBase ? ResolvePlayerProfile(hProcess, moduleBase, moduleSize) : ResolvedProfile{};
    if (resolved.structBase == 0 || profile.xBase == 0) return false;
    equipInventoryData = profile.xBase + kEquipGameDataOffset + kEquipInventoryDataOffset;
    slots = BuildEquipSlotList(profile);
    lastSlotIds.assign(slots.size(), INT32_MIN);  // forces names + equip load to re-resolve
    lastLevel = INT32_MIN;
    lastHandIdx[0] = lastHandIdx[1] = lastHandIdx[2] = INT32_MIN;
    lastSpellIdx = INT32_MIN;
    return true;
  };

  auto readI32 = [&](uintptr_t addr) {
    int32_t v = 0;
    SIZE_T br = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &v, sizeof(v), &br);
    return v;
  };
  auto itemName = [&](int32_t inventoryItemId) -> std::string {
    if (inventoryItemId < 0) return "(empty)";
    auto item = ResolveItem(hProcess, equipInventoryData, inventoryItemId);
    if (!item.ok || !item.plausible) return "(unresolved)";
    std::string name = LookupItemName(ItemNames(), item.uniqueId, item.giveId);
    return name.empty() ? "(name not found)" : name;
  };
  auto slotItemId = [&](const char* slotName) -> int32_t {
    for (size_t i = 0; i < slots.size(); ++i) {
      if (std::string(slots[i].name) == slotName) return lastSlotIds[i];
    }
    return -1;
  };
  // Same categorisation as stats' readAndPrintEquipLoadAndPoise.
  auto recomputeEquipLoad = [&]() {
    std::vector<EquippedWeightItem> items;
    std::vector<int32_t> ringGiveIds;
    for (size_t i = 0; i < slots.size(); ++i) {
      if (lastSlotIds[i] < 0) continue;
      auto item = ResolveItem(hProcess, equipInventoryData, lastSlotIds[i]);
      if (!item.ok || !item.plausible) continue;
      std::string slotName = slots[i].name;
      EquipWeightTable table = EquipWeightTable::kWeapon;
      if (slotName == "Head" || slotName == "Chest" || slotName == "Hands" || slotName == "Legs") {
        table = EquipWeightTable::kProtector;
      } else if (slotName.rfind("Ring", 0) == 0) {
        table = EquipWeightTable::kAccessory;
        ringGiveIds.push_back(item.giveId);
      } else if (slotName == "Covenant") {
        table = EquipWeightTable::kAccessory;
      }
      items.push_back({table, item.giveId, slotName});
    }
    auto load = ComputeCurrentEquipLoad(hProcess, moduleBase, items);
    snap.haveEquipLoad = load.ok;
    snap.equipLoad = load.total;
    snap.maxEquipLoad = ComputeBaseMaxEquipLoad(readI32(profile.vitalityAddress)) *
                        ComputeRingEquipLoadRateMultiplier(hProcess, moduleBase, ringGiveIds);
  };

  PerfWindow perf;
  perf.Reset();
  LARGE_INTEGER qpcFreq, nextTick;
  QueryPerformanceFrequency(&qpcFreq);
  QueryPerformanceCounter(&nextTick);
  const LONGLONG tickInterval =
      static_cast<LONGLONG>(qpcFreq.QuadPart * kPollIntervalMs / 1000.0);
  int ticksSincePerfSample = 0;
  int keyItemTicks = 0;
  int lastSpoilerTier = -1;
  TrailPoint lastTrail{0, 0, 0, -1, 0};
  int32_t lastTrailDeaths = -1, lastTrailBonfire = INT32_MIN;

  while (!g_window.stop) {
    AdoptNewMapData();  // safe point: no reader holds the tables between ticks
    if (g_window.paused) {
      perf.Report();
      if (auto ps = perf.Take(); ps.valid) tracker.log.WritePerf(PerfCsvRow(ps, tracker.stats.seconds));
      Log("PAUSED (F10) -- polling stopped, zero reads until resumed.");
      snap.paused = true;
      PublishSnapshot(snap);
      while (g_window.paused && !g_window.stop) {
        WaitForSingleObject(g_window.wakeEvent, INFINITE);  // blocks: no reads, no CPU
      }
      if (g_window.stop) break;
      Log("RESUMED.");
      snap.paused = false;
      lastSessionTick.QuadPart = 0;  // the paused gap isn't session time
      perf.Reset();
      QueryPerformanceCounter(&nextTick);
    }

    DWORD exitCode = 0;
    if (GetExitCodeProcess(hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
      Log("Game process exited (code %lu) -- polling stopped.", exitCode);
      snap.status = "Game closed -- polling stopped.";
      PublishSnapshot(snap);
      break;
    }

    // Player loaded / unloaded / rebuilt? Re-resolving runs AOB scans,
    // so while it keeps failing (mid-load) it's retried once a second.
    uintptr_t playerIns = ReadCurrentPlayerIns(hProcess, moduleBase, wcmCellOffset);
    if (!inGame || playerIns != resolved.entityAddress) {
      bool wasInGame = inGame;
      inGame = false;
      if ((playerIns != 0 || wcmCellOffset == 0) && GetTickCount64() - lastResolveAttempt >= 1000) {
        lastResolveAttempt = GetTickCount64();
        inGame = resolveAll() &&
                 ReadCurrentPlayerIns(hProcess, moduleBase, wcmCellOffset) == resolved.entityAddress;
        if (inGame) Log("Player loaded -- all addresses (re-)resolved.");
      }
      if (!inGame) {
        if (wasInGame) Log("Player unloaded (main menu or loading) -- reads paused.");
        snap.status = "Not in game (main menu or loading) -- waiting.";
        snap.haveValues = false;
        lastSessionTick.QuadPart = 0;  // menus and loading aren't session time
        PublishSnapshot(snap);
        WaitForSingleObject(g_window.wakeEvent, static_cast<DWORD>(kPollIntervalMs));
        QueryPerformanceCounter(&nextTick);
        continue;
      }
      snap.status.clear();
    }

    // The timed core reads -- same set `stats` reports latency for.
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    snap.hp = readI32(resolved.hpAddress);
    snap.maxHp = readI32(resolved.maxHpAddress);
    snap.fp = readI32(resolved.fpAddress);
    snap.maxFp = readI32(resolved.maxFpAddress);
    snap.stamina = readI32(resolved.staminaAddress);
    snap.maxStamina = readI32(resolved.maxStaminaAddress);
    snap.level = readI32(profile.levelAddress);
    snap.souls = readI32(profile.soulsAddress);
    QueryPerformanceCounter(&t1);
    snap.lastLatencyMicros =
        static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / qpcFreq.QuadPart;
    perf.AddTick(snap.lastLatencyMicros);
    snap.lastReadTime = Timestamp();
    snap.pose = ReadPlayerPose(hProcess, resolved);
    {
      PlayerArea area = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress, gameManCellOffset);
      areaTracker.Update(area);
      if (area.ok) snap.mapNumber = static_cast<int>(area.mapId >> 24);
      snap.untendedWorld = areaTracker.lastRegion >= 400000 && areaTracker.lastRegion < 400100;
      snap.areaName = areaTracker.lastRegion ? areaNames.Region(areaTracker.lastRegion) : "";
      snap.lastBonfireName = area.ok ? areaNames.Bonfire(area.lastBonfireId) : "";
      snap.currentArea = areaInfo.AreaOf(areaTracker.lastRegion);
      GameCounters counters = ReadGameCounters(hProcess, moduleBase, profile.baseACellOffset);
      LARGE_INTEGER nowTick;
      QueryPerformanceCounter(&nowTick);
      double dt = lastSessionTick.QuadPart == 0
                      ? 0.0
                      : (std::min)(1.0, static_cast<double>(nowTick.QuadPart -
                                                             lastSessionTick.QuadPart) /
                                            qpcFreq.QuadPart);
      lastSessionTick = nowTick;
      SessionContext ctx;
      ctx.level = snap.level;
      ctx.souls = snap.souls;
      ctx.deathNum = counters.deathNum;
      ctx.playTimeMs = counters.playTime;
      ctx.area = snap.currentArea;
      ctx.characterName = ReadCharacterName(hProcess, profile.xBase);
      ctx.pose = snap.pose;
      ctx.lastBonfireId = area.lastBonfireId;
      ctx.lastBonfireName = snap.lastBonfireName;
      // Bosses and hands as of the last tick's reads (further down the loop).
      ctx.bosses = sessionBosses;
      ctx.hands[0] = handWeapons[0];
      ctx.hands[1] = handWeapons[1];
      snap.characterName = ctx.characterName;
      if (counters.ok && tracker.Tick(dt, ctx)) {
        Log("%s", session.lastEvent.c_str());
      }
      snap.deathsTotal = counters.deathNum;
      snap.sessionSeconds = session.seconds;
      snap.sessionDeaths = session.deaths;
      snap.soulsLost = session.lost;
      snap.soulsRecovered = session.recovered;
      snap.soulsPerHour = session.SoulsPerHour();
      snap.areaSeconds = session.AreaSeconds(snap.currentArea);
      // Live page trail: a point per unit moved, plus deaths and rests.
      if (snap.pose.ok) {
        TrailPoint pt{snap.pose.x, snap.pose.y, snap.pose.z,
                      static_cast<int16_t>(snap.mapNumber * 2 + (snap.untendedWorld ? 1 : 0)), 0};
        float dx = pt.x - lastTrail.x, dy = pt.y - lastTrail.y, dz = pt.z - lastTrail.z;
        if (pt.mapKey != lastTrail.mapKey || dx * dx + dy * dy + dz * dz >= 1.0f) {
          TrailAdd(pt);
          lastTrail = pt;
        }
        if (lastTrailDeaths >= 0 && session.deaths > lastTrailDeaths) TrailAdd({pt.x, pt.y, pt.z, pt.mapKey, 1});
        lastTrailDeaths = session.deaths;
        if (area.ok && lastTrailBonfire != INT32_MIN && area.lastBonfireId != lastTrailBonfire)
          TrailAdd({pt.x, pt.y, pt.z, pt.mapKey, 2});
        if (area.ok) lastTrailBonfire = area.lastBonfireId;
      }
      // Co-op: this player's shared state. The realtime part every tick;
      // the flag sets further down, every 5s while the page is open.
      {
        std::lock_guard<std::mutex> guard(g_coop.lock);
        SharedGameState& m = g_coop.mine;
        m.capturedUtc = static_cast<uint32_t>(std::time(nullptr));
        m.characterName = snap.characterName;
        m.level = CoopClampU16(snap.level);
        m.hp = CoopClampU16(snap.hp), m.maxHp = CoopClampU16(snap.maxHp);
        m.fp = CoopClampU16(snap.fp), m.maxFp = CoopClampU16(snap.maxFp);
        m.stamina = CoopClampU16(snap.stamina), m.maxStamina = CoopClampU16(snap.maxStamina);
        for (int k = 0; k < 9; ++k) m.attrs[k] = static_cast<uint8_t>((std::min)((std::max)(snap.attrs[k], 0), 255));
        m.souls = static_cast<uint32_t>((std::max)(snap.souls, 0));
        m.deaths = static_cast<uint32_t>((std::max)(counters.deathNum, 0));
        m.playSeconds = counters.playTime / 1000;
        if (area.ok) {
          m.mapId = area.mapId;
          m.lastBonfireId = area.lastBonfireId;
        }
        m.playRegionId = areaTracker.lastRegion;
        m.havePose = snap.pose.ok;
        m.x = snap.pose.x, m.y = snap.pose.y, m.z = snap.pose.z, m.facing = snap.pose.facingRadians;
      }
    }
    snap.nextLevelCost = RequiredSoulsForLevel(snap.level + 1);
    snap.haveValues = true;

    // Gear, level (Vitality -> max load), and active-slot changes: cheap
    // int32 reads every tick, heavier name/weight resolution only on change.
    bool gearChanged = snap.level != lastLevel || refreshGear;
    refreshGear = false;
    lastLevel = snap.level;
    for (size_t i = 0; i < slots.size(); ++i) {
      int32_t id = readI32(slots[i].address);
      if (id != lastSlotIds[i]) gearChanged = true;
      lastSlotIds[i] = id;
    }
    if (gearChanged) recomputeEquipLoad();

    int32_t rightIdx = readI32(profile.xBase + kRightHandSlotOffset);
    int32_t leftIdx = readI32(profile.xBase + kLeftHandSlotOffset);
    int32_t quickIdx = readI32(profile.xBase + kSelectedQuickItemSlotOffset);
    int32_t spellIdx = ReadActiveSpellIndex(hProcess, profile.xBase);
    // The weapon in a hand, for the session log: its param id (upgrade in
    // the last two digits) and name.
    auto handWeapon = [&](int32_t inventoryId) {
      SessionWeapon w;
      ItemLookup item = inventoryId >= 0 ? ResolveItem(hProcess, equipInventoryData, inventoryId) : ItemLookup{};
      if (!item.ok || !item.plausible) return w;
      w.id = item.giveId;
      w.name = itemName(inventoryId);
      w.upgrade = item.giveId % 100;
      return w;
    };
    if (gearChanged || rightIdx != lastHandIdx[0]) {
      std::string slot = "R" + std::to_string(rightIdx + 1);
      snap.rightHand = (rightIdx >= 0 && rightIdx <= 2) ? itemName(slotItemId(slot.c_str())) : "?";
      handWeapons[0] = (rightIdx >= 0 && rightIdx <= 2) ? handWeapon(slotItemId(slot.c_str())) : SessionWeapon();
    }
    if (gearChanged || leftIdx != lastHandIdx[1]) {
      std::string slot = "L" + std::to_string(leftIdx + 1);
      snap.leftHand = (leftIdx >= 0 && leftIdx <= 2) ? itemName(slotItemId(slot.c_str())) : "?";
      handWeapons[1] = (leftIdx >= 0 && leftIdx <= 2) ? handWeapon(slotItemId(slot.c_str())) : SessionWeapon();
    }
    if (gearChanged || quickIdx != lastHandIdx[2]) {
      int32_t id = (quickIdx >= 0 && quickIdx <= 9)
                       ? readI32(profile.xBase + kQuickItemArrayOffset +
                                 static_cast<uintptr_t>(quickIdx) * kQuickItemStride)
                       : -1;
      snap.quickItem = id > 0 ? itemName(id) : "(empty)";
    }
    // Spells can be re-attuned without a gear change, so re-resolve the
    // name on a timer too (once a second) rather than only on index change.
    if (gearChanged || spellIdx != lastSpellIdx || ticksSincePerfSample == 0) {
      int32_t magicId =
          (spellIdx >= 0 && spellIdx < 14) ? ReadSpellSlot(hProcess, profile.xBase, spellIdx + 1) : 0;
      std::string name = magicId > 0 ? LookupSpellName(ItemNames(), magicId) : "";
      snap.spell = magicId <= 0 ? "(none)" : (name.empty() ? "(name not found)" : name);
    }
    lastHandIdx[0] = rightIdx;
    lastHandIdx[1] = leftIdx;
    lastHandIdx[2] = quickIdx;
    lastSpellIdx = spellIdx;

    // Live page: attributes and the six weapon slots (name, attack rating,
    // next upgrade). AR reads several param rows, so only on a gear or
    // level change; which weapon is in hand is cheap and checked per tick.
    if (gearChanged) {
      const uintptr_t attrAddr[9] = {profile.vigorAddress,    profile.attunementAddress,
                                     profile.enduranceAddress, profile.vitalityAddress,
                                     profile.strengthAddress,  profile.dexterityAddress,
                                     profile.intelligenceAddress, profile.faithAddress,
                                     profile.luckAddress};
      for (int i = 0; i < 9; ++i) snap.attrs[i] = readI32(attrAddr[i]);
      snap.weapons.clear();
      const auto held = HeldGoodsQty(hProcess, profile);  // once for all six upgrade lines
      std::vector<int32_t> materials;
      for (const char* slotName : {"R1", "R2", "R3", "L1", "L2", "L3"}) {
        int32_t id = slotItemId(slotName);
        if (id < 0) continue;
        auto item = ResolveItem(hProcess, equipInventoryData, id);
        if (!item.ok || !item.plausible || item.giveId == 110000) continue;  // empty / Fists
        LiveWeapon w;
        w.slot = slotName;
        w.name = itemName(id);
        auto ar = ComputeWeaponAr(hProcess, moduleBase, item.giveId, snap.attrs[4], snap.attrs[5], snap.attrs[6],
                                  snap.attrs[7], snap.attrs[8]);
        if (ar.ok) {
          w.haveAr = true;
          w.catalyst = ar.isCatalyst;
          w.spellBuff = ar.spellBuff;
          for (int t = 0; t < 5 && t < kAecTypeCount; ++t) {
            w.present[t] = ar.types[t].present;
            w.base[t] = ar.types[t].base;
            w.bonus1h[t] = ar.types[t].bonus1H;
            w.bonus2h[t] = ar.types[t].bonus2H;
          }
        }
        // Level planner: the same AR with each offensive stat one higher.
        for (int k = 0; k < 5 && ar.ok; ++k) {
          int32_t a[5] = {snap.attrs[4], snap.attrs[5], snap.attrs[6], snap.attrs[7], snap.attrs[8]};
          a[k] += 1;
          auto up = ComputeWeaponAr(hProcess, moduleBase, item.giveId, a[0], a[1], a[2], a[3], a[4]);
          if (!up.ok) continue;
          for (int t = 0; t < kAecTypeCount; ++t) {
            if (!ar.types[t].present) continue;
            w.plan1h[k] += up.types[t].bonus1H - ar.types[t].bonus1H;
            w.plan2h[k] += up.types[t].bonus2H - ar.types[t].bonus2H;
            w.planFix[k] |= ar.types[t].penalized1H && !up.types[t].penalized1H;
          }
          w.planBuff[k] = up.spellBuff - ar.spellBuff;
        }
        // Every infusion of this weapon at the same level and current stats.
        // Rows: base + 100 x infusion (0 Normal ... 15 Hollow); weapons that
        // can't be infused have only the +0 row.
        {
          const int32_t level = item.giveId % 100, weaponBase = item.giveId - item.giveId % 10000;
          int rows = 0;
          for (int k = 0; k < 16; ++k) {
            w.inf1h[k] = w.inf2h[k] = -1;
            auto r = ComputeWeaponAr(hProcess, moduleBase, weaponBase + 100 * k + level, snap.attrs[4], snap.attrs[5],
                                     snap.attrs[6], snap.attrs[7], snap.attrs[8]);
            if (!r.ok) continue;
            ++rows;
            if (r.isCatalyst) {
              w.inf1h[k] = w.inf2h[k] = r.spellBuff;
              continue;
            }
            w.inf1h[k] = w.inf2h[k] = 0;
            for (int t = 0; t < kAecTypeCount; ++t) {
              if (!r.types[t].present) continue;
              w.inf1h[k] += r.types[t].base + r.types[t].bonus1H;
              w.inf2h[k] += r.types[t].base + r.types[t].bonus2H;
            }
          }
          w.infCurrent = rows > 1 ? (item.giveId % 10000) / 100 : -1;
        }
        w.upgrade = WindowUpgrade(hProcess, moduleBase, profile, id, held, &materials);
        snap.weapons.push_back(w);
      }
      upgradeMaterials.clear();
      for (int32_t m : materials) {
        auto it = held.find(m);
        upgradeMaterials[m] = it == held.end() ? 0 : it->second;
      }
    }
    {
      int32_t sheath = readI32(profile.xBase + kWeaponSheathStateOffset);
      for (auto& w : snap.weapons) {
        bool right = w.slot[0] == 'R';
        int idx = w.slot[1] - '1';
        w.active = right ? idx == rightIdx : idx == leftIdx;
        w.twoHanded = w.active && ((right && sheath == 3) || (!right && sheath == 2));
      }
    }

    // World pickups found in this map section: once a second (~30-40
    // flag reads). The pickup list itself is cached per map section.
    bool tierChanged = SpoilerTier() != lastSpoilerTier;
    lastSpoilerTier = SpoilerTier();
    if (ticksSincePerfSample == 0 || tierChanged) {
      PlayerArea a = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress, 0);
      if (a.ok) {
        // Per named area (pickup positions + play regions); falls back to
        // the map section when the area has no positioned pickups.
        int total = 0;
        int found = CountFoundInArea(hProcess, moduleBase, moduleSize, profile.baseACellOffset,
                                     snap.currentArea, &total);
        snap.itemsScope = found >= 0 ? "in " + snap.currentArea : "in this map section";
        if (found < 0)
          found = CountFoundPickups(hProcess, moduleBase, moduleSize, profile.baseACellOffset,
                                    static_cast<int>(a.mapId >> 24), &total);
        snap.itemsFound = found;
        snap.itemsTotal = total;
      }
      WindowProgress(hProcess, moduleBase, moduleSize, profile.baseACellOffset, snap.currentArea,
                     &snap.bossesHere, &snap.bossesDefeated, &snap.bossesTotal, &snap.bossesLater, &snap.shards);
      {
        // An upgrade changes a weapon's id, not its slot: re-resolve gear then.
        int k = 0;
        for (const char* slotName : {"R1", "R2", "R3", "L1", "L2", "L3"}) {
          int32_t id = slotItemId(slotName);
          ItemLookup item = id >= 0 ? ResolveItem(hProcess, equipInventoryData, id) : ItemLookup{};
          int32_t weaponId = item.ok && item.plausible ? item.giveId : -1;
          if (weaponId != lastWeaponIds[k]) refreshGear = true;
          lastWeaponIds[k++] = weaponId;
        }
      }
      if (snap.pose.ok) {
        PlayerArea where = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress, 0);
        where.playRegionId = areaTracker.lastRegion;  // keep the last region between regions
        WindowNearest(hProcess, moduleBase, moduleSize, profile.baseACellOffset, snap.pose, where,
                      &snap.nearest, &snap.nearestShort);
        LiveNearbyList(hProcess, moduleBase, moduleSize, profile.baseACellOffset, snap.pose, where,
                       snap.currentArea, &snap.nearby);
      }
      LiveBossList(hProcess, moduleBase, moduleSize, snap.currentArea, snap.pose, &snap.bosses, snap.bossCounts,
                   &sessionBosses);
      LiveRoute(hProcess, moduleBase, moduleSize, profile.baseACellOffset, snap.characterName, snap.currentArea,
                &snap.route, &snap.routeHint, &snap.routeHidden);
      LiveMissables(hProcess, moduleBase, moduleSize, profile.baseACellOffset, snap.characterName, snap.currentArea,
                    &snap.missables, &snap.missableAlert, &snap.missablesHidden);
      BuildLevelPlan(hProcess, moduleBase, &snap);
      snap.bossHint = WindowBossHint(hProcess, moduleBase, moduleSize, snap.currentArea, snap.pose);
      // Key items walk the inventory, so every 5s rather than every second.
      if (++keyItemTicks % 5 == 1) {
        snap.keyItems = WindowKeyItems(hProcess, moduleBase, moduleSize, profile);
        // Infusion gems held: Heavy Gem 1100, Sharp 1110 ... Hollow 1240 (goods).
        auto held = HeldGoodsQty(hProcess, profile);
        snap.gemHeld[0] = true;
        for (int k = 1; k < 16; ++k) snap.gemHeld[k] = held.count(1100 + (k - 1) * 10) > 0;
        // An upgrade material picked up or spent: the next-upgrade lines change.
        for (const auto& [goods, had] : upgradeMaterials) {
          auto it = held.find(goods);
          if ((it == held.end() ? 0 : it->second) != had) refreshGear = true;
        }
        int32_t rIdx = readI32(profile.xBase + kRightHandSlotOffset);
        std::string slot = "R" + std::to_string(rIdx + 1);
        int32_t invId = (rIdx >= 0 && rIdx <= 2) ? slotItemId(slot.c_str()) : -1;
        snap.upgrade = invId >= 0 ? WindowUpgrade(hProcess, moduleBase, profile, invId, held) : "";
        // Co-op flag sets (~1,000 pickup flags), only while the page is open
        // or a co-op session needs them for the partner.
        if (LivePageRecentlyPolled() || CoopSessionActive()) {
          SharedGameState flags;
          CoopLists lists;
          ReadMyCoopFlags(hProcess, moduleBase, moduleSize, profile, &flags, &lists);
          std::lock_guard<std::mutex> guard(g_coop.lock);
          g_coop.mine.bossesDefeated = flags.bossesDefeated;
          g_coop.mine.keyItemsObtained = flags.keyItemsObtained;
          g_coop.mine.keyItemsHeld = flags.keyItemsHeld;
          g_coop.mine.pickupsFound = flags.pickupsFound;
          g_coop.mine.clearCount = flags.clearCount;
          g_coop.lists = std::move(lists);
          g_coop.mineReady = true;
        }
      }
    }
    // Perf figures: refreshed once a second over a rolling 5s window.
    if (++ticksSincePerfSample >= 10) {
      ticksSincePerfSample = 0;
      snap.perf = perf.Take();
      if (perf.ElapsedSeconds() >= kPerfReportIntervalSeconds) {
        if (snap.perf.valid) tracker.log.WritePerf(PerfCsvRow(snap.perf, tracker.stats.seconds));
        perf.Reset();
      }
    }
    PublishSnapshot(snap);

    // Fixed-deadline 10Hz, same as stats --live; the wait wakes early on
    // a pause toggle or shutdown.
    nextTick.QuadPart += tickInterval;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart - nextTick.QuadPart > tickInterval) nextTick = now;
    LONGLONG waitTicks = nextTick.QuadPart - now.QuadPart;
    DWORD waitMs = waitTicks > 0 ? static_cast<DWORD>(waitTicks * 1000 / qpcFreq.QuadPart) : 0;
    WaitForSingleObject(g_window.wakeEvent, waitMs);
  }
  tracker.Finish();
  CloseHandle(hProcess);
  if (session.seconds > 0) {
    Log("Session: %s, %d death(s), %.0f souls/h, lost %lld, recovered %lld.", FormatDuration(session.seconds).c_str(),
        session.deaths, session.SoulsPerHour(), static_cast<long long>(session.lost),
        static_cast<long long>(session.recovered));
  }
}

// Waits for the game, attaches, polls until it closes, then waits again --
// so the overlay and live page can be started before the game and keep
// running across game restarts. While waiting it only lists processes
// (every 2s); it attaches through AttachReadOnly, which still refuses any
// DS3 without the Seamless Co-op module (never vanilla online).
DWORD WINAPI WindowPollWorker(LPVOID) {
  bool announced = false;
  DWORD refusedPid = 0;
  while (!g_window.stop) {
    DWORD pid = FindProcessIdByName(kTargetProcessName);
    bool coop = pid != 0 && HasModule(ListModuleNames(pid), kSeamlessCoopModuleMarker);
    if (pid != 0 && !coop && pid != refusedPid) {
      Log("Found Dark Souls III (PID %lu) without Seamless Co-op loaded -- not attaching (vanilla online is "
          "never touched); waiting.", pid);
      refusedPid = pid;
    }
    HANDLE hProcess = nullptr;
    if (coop) {
      DWORD attachedPid = 0;
      hProcess = AttachReadOnly(&attachedPid);
      if (hProcess) pid = attachedPid;
    }
    if (!hProcess) {
      if (!announced) Log("Waiting for Dark Souls III (Seamless Co-op) to start...");
      announced = true;
      WindowSnapshot waiting;
      waiting.status = "Waiting for Dark Souls III (Seamless Co-op) to start...";
      PublishSnapshot(waiting);
      WaitForSingleObject(g_window.wakeEvent, 2000);
      continue;
    }
    announced = false;
    refusedPid = 0;
    PollGameRun(hProcess, pid);
  }
  return 0;
}

