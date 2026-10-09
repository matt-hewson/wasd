// Directions to pickups and which "world" (Untended Graves vs. the
// normal Cemetery/Firelink) a pickup belongs to.

// Facing convention, live-verified 2026-10-06 (docs/TECHNICAL.md, "Clock
// direction"): the facing angle points the opposite way to atan2(dx, dz),
// so "ahead" is the direction at angle facing - pi.
PlayerPose FacingPose(double facing) {
  PlayerPose p;
  p.ok = true;
  p.facingRadians = static_cast<float>(facing);
  return p;
}

std::string ClockAt(double facing, double angleFromAhead) {
  const double kPi = 3.141592653589793;
  double a = facing - kPi + angleFromAhead;  // world angle of the target, as atan2(dx, dz)
  return ClockDirection(FacingPose(facing), static_cast<float>(std::sin(a) * 10),
                        static_cast<float>(std::cos(a) * 10));
}

TEST(clock_direction_ahead_behind_and_sides_at_any_facing) {
  const double kPi = 3.141592653589793;
  for (double facing : {0.0, 0.5, 1.2, kPi, -2.0, 3.0}) {
    CHECK_EQ(ClockAt(facing, 0), std::string("12 o'clock"));
    CHECK_EQ(ClockAt(facing, kPi / 2), std::string("3 o'clock"));
    CHECK_EQ(ClockAt(facing, kPi), std::string("6 o'clock"));
    CHECK_EQ(ClockAt(facing, -kPi / 2), std::string("9 o'clock"));
    CHECK_EQ(ClockAt(facing, kPi / 6), std::string("1 o'clock"));
  }
}

TEST(clock_direction_live_verified_axis) {
  // Facing 0 looks down -z; +x is then on the left (9 o'clock). This is the
  // orientation the 180-degree fix of 2026-10-06 settled on.
  CHECK_EQ(ClockDirection(FacingPose(0), 0, -5), std::string("12 o'clock"));
  CHECK_EQ(ClockDirection(FacingPose(0), 5, 0), std::string("9 o'clock"));
  CHECK_EQ(ClockDirection(FacingPose(0), -5, 0), std::string("3 o'clock"));
}

TEST(relative_bearing_stays_in_range) {
  for (double facing = -7; facing <= 7; facing += 0.37)
    for (double a = -7; a <= 7; a += 0.41) {
      double b = RelativeBearing(FacingPose(facing), static_cast<float>(std::sin(a)), static_cast<float>(std::cos(a)));
      CHECK(b >= 0 && b < 6.2832);
    }
}

TEST(untended_graves_pickups_only_in_their_world) {
  const int32_t untended = 400050, cemetery = 400150, highWall = 300001;
  CHECK(SameWorld(untended, 400010));    // both in Untended Graves
  CHECK(!SameWorld(untended, cemetery));  // Untended pickup, player in the normal world
  CHECK(!SameWorld(cemetery, 400010));    // normal pickup, player in Untended Graves
  CHECK(SameWorld(cemetery, cemetery));
  CHECK(SameWorld(cemetery, 0));          // unknown player region = the normal world
  CHECK(!SameWorld(untended, 0));
  CHECK(SameWorld(highWall, 400010));     // other maps aren't split
}
