// ---------------------------------------------------------------- the room's options (the Extra Settings page)
// The room's Extra Settings button opens CSNZ_Server's room options page (web/room); the host sets these per room
// (and saves them as the defaults of the rooms they create). /api/matchbots hands them to botassist with the room's
// AI players ("options"):
//  - orderCount: how many bots take an order (radio, chat, the scenario menu): 0 every bot of the team, else the N
//    alive ones nearest to the player giving it (the others keep what they were doing)
//  - groups: zombie modes, human bots hold several spots in groups (zombie_holdout.inl); off, they all hold one
//  - walkers: zombie modes, a few humans walk the map alone
//  - watchBack: human bots turn to the nearest zombie in sight (zombie_humans.inl ZombieAwareAim)
//  - voices: radio | normal | off; empty: botassist.ini Voices
// Without them (an older server, no room) every option keeps its old behaviour.
struct RoomOptions
{
    int orderCount = 0;
    bool groups = true;
    bool walkers = true;
    bool watchBack = true;
    std::string voices;
};
static RoomOptions g_roomOpts;

static void SetRoomOptions(const std::string& json)
{
    RoomOptions o;
    size_t at = json.find("\"options\":{");
    if (at != std::string::npos)
    {
        size_t end = json.find('}', at);
        std::string obj = json.substr(at + 10, end == std::string::npos ? std::string::npos : end - at - 9);
        o.orderCount = (std::max)(0, (std::min)(31, JsonInt(obj, "orderCount", 0)));
        o.groups = JsonInt(obj, "groups", 1) != 0;
        o.walkers = JsonInt(obj, "walkers", 1) != 0;
        o.watchBack = JsonInt(obj, "watchBack", 1) != 0;
        o.voices = JsonStr(obj, "voices");
        if (o.voices != "radio" && o.voices != "normal" && o.voices != "off")
            o.voices.clear();
    }
    g_roomOpts = o;
    char orders[32];
    snprintf(orders, sizeof(orders), o.orderCount ? "the %d nearest bots" : "every bot", o.orderCount);
    Log("room options: orders reach %s, holdout groups %s, walkers %s, watch their back %s, voices %s", orders,
        o.groups ? "on" : "off (one spot)", o.walkers ? "on" : "off", o.watchBack ? "on" : "off",
        o.voices.empty() ? (cfg.voices + " (botassist.ini)").c_str() : o.voices.c_str());
}

// the bots' voices for this match: the room's, else botassist.ini's
static const std::string& Voices()
{
    return g_roomOpts.voices.empty() ? cfg.voices : g_roomOpts.voices;
}

// the bots an order of `by` reaches (orderCount): its team's alive bots nearest to it; every bot when it's 0
static std::vector<void*> OrderReach(void* by, int team, const std::vector<void*>& bots)
{
    if (!g_roomOpts.orderCount || (int)bots.size() <= g_roomOpts.orderCount)
        return bots;
    V3 me = Origin(by);
    std::vector<std::pair<float, void*>> byDist;
    for (void* e : bots)
        if (Alive(e))
            byDist.push_back({ Len(Origin(e) - me), e });
    std::sort(byDist.begin(), byDist.end(), [](const std::pair<float, void*>& a, const std::pair<float, void*>& b) { return a.first < b.first; });
    std::vector<void*> out;
    for (size_t i = 0; i < byDist.size() && (int)i < g_roomOpts.orderCount; i++)
        out.push_back(byDist[i].second);
    return out;
}
