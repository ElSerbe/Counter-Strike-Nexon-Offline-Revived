// botassist: bots in each other's way, the decisions on their own (no game calls, so a test builds this file alone).
// Included by botassist.cpp before the navigation code; the game side (who is on a ladder, who blocks whom) is there.
//
//  - ladders: one bot climbs at a time each way. A second one follows a bot going the same way once it is LADDER_GAP
//    past their end; one coming the other way (or stopped on it) waits till it's off. Bots walking up to the ladder
//    take turns by distance to it (within LADDER_TIE: by key), so two arriving together don't both go. One at the foot
//    that doesn't get on loses its turn (LadderTurnLost); a bot gives up waiting after LadderWaitLimit (longer the more
//    are ahead of it).

// a player near a ladder: on it (dir 1 up, -1 down, 0 not moving; feetZ), or walking up to it (dist from the ladder)
struct LadderOther
{
    bool on;
    int dir;
    float feetZ;
    float dist;
    long long key;
    bool lost = false; // walking up to it: lost its turn (LadderTurnLost), nobody waits for it
};
static const float LADDER_GAP = 80.0f, LADDER_TIE = 8.0f;

// myDir: the way this bot climbs (1 up, -1 down); entryZ: the height of the end it gets on at; myDist: how far it is from
// the ladder; myKey: its turn when as near as another
static bool LadderMustWait(int myDir, float myDist, long long myKey, float entryZ, const std::vector<LadderOther>& others)
{
    for (const LadderOther& o : others)
    {
        if (o.on)
        {
            if (o.dir != myDir || fabsf(o.feetZ - entryZ) <= LADDER_GAP)
                return true; // coming the other way, stopped, or still at this end
            continue;
        }
        if (o.lost)
            continue; // at the foot without getting on: the next one goes
        if (o.dist + LADDER_TIE < myDist || (fabsf(o.dist - myDist) <= LADDER_TIE && o.key < myKey))
            return true; // nearer the ladder: its turn first
    }
    return false;
}

// A bot at the foot of a ladder that doesn't get on (zm_chosun: one tried to get on at the top 12 times in 15 s) held
// the line behind it till each one gave up. One within LADDER_FOOT of the ladder, not waiting itself, that got no
// closer (LADDER_CLOSER, the game side) for LADDER_TURN_TIME has lost its turn: it keeps trying, the next one goes.
// bestAt: when it last got closer.
static const float LADDER_FOOT = 64.0f, LADDER_TURN_TIME = 3.0f, LADDER_CLOSER = 16.0f;
static bool LadderTurnLost(bool waiting, float gap, float bestAt, float now)
{
    return !waiting && gap < LADDER_FOOT && now - bestAt > LADDER_TURN_TIME;
}

// a bot back at the same ladder within LADDER_KEEP keeps its clock (and its nearest so far): every try to get on takes
// it out of the line for a moment, and starting the clock again each time let one trying every second never lose its
// turn (zm_chosun: up to 5 "lost its turn" each, 14% of the waits still given up). lastAt: its last move in the line.
static const float LADDER_KEEP = 5.0f;
static bool LadderClockKeeps(int oldLadder, int ladder, float lastAt, float now)
{
    return oldLadder == ladder && now >= lastAt && now - lastAt <= LADDER_KEEP;
}

// the bots walking up to the ladder whose turn comes before this one's (the ones LadderMustWait waits for, not on it)
static int LadderAhead(float myDist, long long myKey, const std::vector<LadderOther>& others)
{
    int n = 0;
    for (const LadderOther& o : others)
        n += !o.on && !o.lost &&
             (o.dist + LADDER_TIE < myDist || (fabsf(o.dist - myDist) <= LADDER_TIE && o.key < myKey));
    return n;
}

// how long a bot waits for its turn before it goes another way: 8 s, and 3 s more for each one ahead of it, at most
// 20 s (at round start 5 bots wanted one ladder, and the last ones gave up while the line was moving)
static const float LADDER_WAIT_MIN = 8.0f, LADDER_WAIT_EACH = 3.0f, LADDER_WAIT_CAP = 20.0f;
static float LadderWaitLimit(int ahead)
{
    float t = LADDER_WAIT_MIN + LADDER_WAIT_EACH * (float)(ahead > 0 ? ahead : 0);
    return t < LADDER_WAIT_CAP ? t : LADDER_WAIT_CAP;
}
