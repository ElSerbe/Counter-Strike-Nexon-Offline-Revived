// botassist: bots in each other's way, the decisions on their own (no game calls, so a test builds this file alone).
// Included by botassist.cpp before the navigation code; the game side (who is on a ladder, who blocks whom) is there.
//
//  - ladders: one bot climbs at a time each way. A second one follows a bot going the same way once it is LADDER_GAP
//    past their end; one coming the other way (or stopped on it) waits till it's off. Bots walking up to the ladder
//    take turns by distance to it (within LADDER_TIE: by key), so two arriving together don't both go. One at the foot
//    that doesn't get on loses its turn (LadderTurnLost); a bot gives up waiting after LadderWaitLimit (longer the more
//    are ahead of it).
//  - holdout places: a bot takes the place none of its group stands on (the fewest, the lowest first), so a bot that
//    joins doesn't walk onto one that's taken when an earlier one was infected; the places are points inside the
//    spot's areas (GridPoints) that SpreadPoints keeps apart (HOLD_PLACE_GAP).
//  - in each other's way (WayDecision): someone standing still is gone around like a wall; of two moving bots, the one
//    with the lower key keeps going and the other steps aside; a moving person always gets the way.
//  - blocked links: a link between two areas that stuck bots ran into a wall on, LINK_BLOCK_HITS times, is one every
//    bot's path goes around (NavBlockCost in botassist.cpp) till it's kept no longer.

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

// the place (0 .. places-1) the fewest of taken stand on, the lowest of those
static int LeastUsedPlace(const std::vector<int>& taken, int places)
{
    if (places <= 0)
        return 0;
    std::vector<int> used(places, 0);
    for (int t : taken)
        if (t >= 0 && t < places)
            used[t]++;
    int best = 0;
    for (int i = 1; i < places; i++)
        if (used[i] < used[best])
            best = i;
    return best;
}

// in order: the points at least gap from every point kept before them (their indices)
static std::vector<int> SpreadPoints(const std::vector<std::pair<float, float>>& pts, float gap)
{
    std::vector<int> kept;
    for (int i = 0; i < (int)pts.size(); i++)
    {
        bool apart = true;
        for (int k : kept)
        {
            float dx = pts[i].first - pts[k].first, dy = pts[i].second - pts[k].second;
            apart = apart && dx * dx + dy * dy >= gap * gap;
        }
        if (apart)
            kept.push_back(i);
    }
    return kept;
}

// what this bot does about the player in its way. Someone standing still (a bot holding its place, a person not moving)
// isn't going to step aside: it's in the way like a wall, so this bot goes around (keeping going into it was a
// deadlock: de_dust, a bot pushed into one holding its holdout place 36 times in a row). Of two moving bots the one with
// the lower key keeps going and the other steps aside; a moving person (or a bot botassist doesn't drive) gets the way.
// otherIsBot: a bot botassist drives
enum
{
    WAY_KEEP_GOING,
    WAY_STEP_ASIDE,
    WAY_GO_AROUND
};
static int WayDecision(bool otherIsBot, bool otherMoving, long long myKey, long long otherKey)
{
    if (!otherMoving)
        return WAY_GO_AROUND;
    return !otherIsBot || otherKey < myKey ? WAY_STEP_ASIDE : WAY_KEEP_GOING;
}

// points to stand on in a nav area's rectangle (lo, hi): step apart, at least inset from its edges; along a side too
// narrow for both insets, its middle. Holdout places come from these, not the areas' middles: de_dust's big areas left
// a spot 1 or 2 places for 5 bots.
static std::vector<std::pair<float, float>> GridPoints(float loX, float loY, float hiX, float hiY, float step, float inset)
{
    auto line = [&](float lo, float hi) {
        std::vector<float> v;
        if (hi - lo < 2 * inset || step <= 0)
            v.push_back((lo + hi) * 0.5f);
        else
            for (float x = lo + inset; x <= hi - inset + 0.01f; x += step)
                v.push_back(x);
        return v;
    };
    std::vector<std::pair<float, float>> out;
    for (float y : line(loY, hiY))
        for (float x : line(loX, hiX))
            out.push_back({ x, y });
    return out;
}

// a link between two nav areas that a bot couldn't walk (a wall or door the mesh doesn't know about): from LINK_BLOCK_HITS
// stuck reports on, every bot's path goes another way while there is one. ent/cls: what was in the way (only the game side
// reads them: once that's gone, a broken breakable, the link is open again)
struct BlockedLink
{
    int from, to, hits;
    float last, until;
    void* ent;
    int cls;
};
static const int LINK_BLOCK_HITS = 2, LINK_BLOCK_MAX = 64;

// one more stuck report on from -> to: counted, and kept keep s from now. A full list drops the one ending first.
static void NoteBlockedLink(std::vector<BlockedLink>& links, int from, int to, float now, float keep, void* ent, int cls)
{
    for (BlockedLink& l : links)
        if (l.from == from && l.to == to)
        {
            l.hits++;
            l.last = now;
            l.until = now + keep;
            l.ent = ent;
            l.cls = cls;
            return;
        }
    if ((int)links.size() >= LINK_BLOCK_MAX)
    {
        size_t first = 0;
        for (size_t i = 1; i < links.size(); i++)
            if (links[i].until < links[first].until)
                first = i;
        links.erase(links.begin() + first);
    }
    links.push_back({ from, to, 1, now, now + keep, ent, cls });
}

// from -> to blocked for everyone now (a clock that went back, a new map, doesn't count)
static bool LinkBlocked(const std::vector<BlockedLink>& links, int from, int to, float now)
{
    for (const BlockedLink& l : links)
        if (l.from == from && l.to == to)
            return l.hits >= LINK_BLOCK_HITS && now >= l.last && now < l.until;
    return false;
}
