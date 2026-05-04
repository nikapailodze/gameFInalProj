#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")
#pragma comment(lib, "winmm")

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr float TileSize = 48.0f;
    constexpr float WindowWidth = 1280.0f;
    constexpr float WindowHeight = 720.0f;
    constexpr float Gravity = 1950.0f;
    constexpr float MoveSpeed = 340.0f;
    constexpr float JumpVelocity = -860.0f;
    constexpr float EnemySpeed = 95.0f;
    constexpr float FixedDt = 1.0f / 60.0f;

    struct Vec2
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Rect
    {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
    };

    bool Contains(const Rect& r, float px, float py)
    {
        return px >= r.x && px <= r.x + r.w &&
            py >= r.y && py <= r.y + r.h;
    }

    bool Intersects(const Rect& a, const Rect& b)
    {
        return a.x < b.x + b.w && a.x + a.w > b.x &&
            a.y < b.y + b.h && a.y + a.h > b.y;
    }

    D2D1_RECT_F ToD2D(const Rect& r, const Vec2& camera)
    {
        return D2D1::RectF(r.x - camera.x, r.y - camera.y, r.x + r.w - camera.x, r.y + r.h - camera.y);
    }

    struct Coin
    {
        Vec2 pos;
        bool collected = false;
    };

    struct Checkpoint
    {
        Vec2 pos;
        bool active = false;
    };

    struct Enemy
    {
        Rect body;
        float vx = EnemySpeed;
        bool alive = true;
    };

    struct MovingPlatform
    {
        Rect body;
        float minX = 0.0f;
        float maxX = 0.0f;
        float vx = 0.0f;
    };

    struct Player
    {
        Rect body;
        Vec2 velocity;
        bool onGround = false;
        int jumpsUsed = 0;
        int lives = 3;
        int coins = 0;
    };

    struct JumpParticle
    {
        Vec2 pos;
        Vec2 velocity;
        float life = 0.0f;
        float maxLife = 0.0f;
    };

    struct RoundStyle
    {
        UINT32 sky;
        UINT32 cloud;
        UINT32 ground;
        UINT32 tile;
        UINT32 tileTop;
        UINT32 exit;
        UINT32 exitGlow;
        UINT32 enemy;
    };

    class Game
    {
    public:
        void LoadLevel(const std::wstring& path)
        {
            std::ifstream file(path);
            std::vector<std::string> loaded;
            std::string line;

            while (std::getline(file, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }

                if (!line.empty())
                {
                    loaded.push_back(line);
                }
            }

            if (loaded.empty())
            {
                loaded = BuiltInRound(0);
            }

            firstRoundRows = loaded;
            LoadBestScore();
            LoadRound(0);
            state = State::Menu;
        }

        void LoadRound(int index)
        {
            currentRound = std::clamp(index, 0, RoundCount() - 1);
            player.coins = 0;
            roundTime = 0.0f;
            storyBannerTimer = 3.5f;
            LoadRows(currentRound == 0 ? firstRoundRows : BuiltInRound(currentRound));
        }

        int RoundCount() const
        {
            return 3;
        }

        void LoadRows(const std::vector<std::string>& loaded)
        {
            rows = loaded;
            width = 0;
            for (const auto& row : rows)
            {
                width = std::max(width, static_cast<int>(row.size()));
            }
            height = static_cast<int>(rows.size());

            coins.clear();
            enemies.clear();
            spikes.clear();
            checkpoints.clear();
            movingPlatforms.clear();
            exitRect = {};

            Vec2 spawn = { TileSize * 2.0f, TileSize * 2.0f };

            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < static_cast<int>(rows[y].size()); ++x)
                {
                    const char tile = rows[y][x];
                    const Vec2 pos = { x * TileSize, y * TileSize };

                    if (tile == 'P')
                    {
                        spawn = { pos.x + 7.0f, pos.y + 4.0f };
                        rows[y][x] = '.';
                    }
                    else if (tile == 'C')
                    {
                        coins.push_back({ { pos.x + TileSize * 0.5f, pos.y + TileSize * 0.5f }, false });
                        rows[y][x] = '.';
                    }
                    else if (tile == 'E')
                    {
                        enemies.push_back({ { pos.x + 6.0f, pos.y + 8.0f, 36.0f, 40.0f }, RoundEnemySpeed(), true });
                        rows[y][x] = '.';
                    }
                    else if (tile == 'K')
                    {
                        checkpoints.push_back({ { pos.x + 10.0f, pos.y + 2.0f }, false });
                        rows[y][x] = '.';
                    }
                    else if (tile == '^')
                    {
                        spikes.push_back({ pos.x + 4.0f, pos.y + 18.0f, TileSize - 8.0f, TileSize - 18.0f });
                        rows[y][x] = '.';
                    }
                    else if (tile == 'M')
                    {
                        movingPlatforms.push_back({
                            { pos.x, pos.y + 30.0f, TileSize, 14.0f },
                            pos.x - TileSize * 2.0f,
                            pos.x + TileSize * 2.0f,
                            90.0f
                        });
                        rows[y][x] = '.';
                    }
                    else if (tile == 'X')
                    {
                        exitRect = { pos.x + 8.0f, pos.y + 4.0f, 32.0f, 44.0f };
                        rows[y][x] = '.';
                    }
                }
            }

            initialSpawn = spawn;
            checkpointSpawn = spawn;
            ResetRun();
        }

        void ResetRun()
        {
            player.body = { checkpointSpawn.x, checkpointSpawn.y, 34.0f, 44.0f };
            player.velocity = {};
            player.onGround = false;
            player.jumpsUsed = 0;
            jumpParticles.clear();
            jumpAnimTimer = 0.0f;
            state = State::Playing;
        }

        void RestartAll()
        {
            player.lives = 3;
            player.coins = 0;
            currentRound = 0;
            totalRunTime = 0.0f;
            finalCompletionTime = -1.0f;
            LoadRound(0);
        }

        void HandleClick(float screenX, float screenY)
        {
            if (state != State::Menu)
            {
                return;
            }

            if (Contains(StartButtonRect(), screenX, screenY))
            {
                RestartAll();
                state = State::Playing;
                return;
            }

            const auto swatches = ColorSwatches();
            for (int i = 0; i < static_cast<int>(swatches.size()); ++i)
            {
                if (Contains(swatches[i], screenX, screenY))
                {
                    selectedPlayerColor = i;
                    return;
                }
            }
        }

        void RestartCurrentRound()
        {
            player.coins = 0;
            roundTime = 0.0f;
            checkpointSpawn = initialSpawn;
            for (auto& coin : coins)
            {
                coin.collected = false;
            }
            for (auto& checkpoint : checkpoints)
            {
                checkpoint.active = false;
            }
            for (auto& enemy : enemies)
            {
                enemy.alive = true;
                enemy.vx = enemy.vx < 0.0f ? -RoundEnemySpeed() : RoundEnemySpeed();
            }
            for (auto& platform : movingPlatforms)
            {
                platform.body.x = (platform.minX + platform.maxX) * 0.5f;
                platform.vx = std::abs(platform.vx);
            }
            ResetRun();
        }

        void Update(float dt, const bool* keys)
        {
            elapsed += dt;

            if (state == State::Menu)
            {
                return;
            }

            if (state != State::Playing)
            {
                if (Pressed(keys, 'R'))
                {
                    RestartAll();
                }
                return;
            }

            roundTime += dt;
            totalRunTime += dt;
            jumpAnimTimer = std::max(0.0f, jumpAnimTimer - dt);
            storyBannerTimer = std::max(0.0f, storyBannerTimer - dt);
            UpdateJumpParticles(dt);
            float direction = 0.0f;
            if (Down(keys, 'A') || Down(keys, VK_LEFT))
            {
                direction -= 1.0f;
            }
            if (Down(keys, 'D') || Down(keys, VK_RIGHT))
            {
                direction += 1.0f;
            }

            player.velocity.x = direction * MoveSpeed;

            const bool jump = Pressed(keys, VK_SPACE) || Pressed(keys, 'W') || Pressed(keys, VK_UP);
            if (jump && player.jumpsUsed < 2)
            {
                player.velocity.y = JumpVelocity;
                player.onGround = false;
                ++player.jumpsUsed;
                TriggerJumpEffects();
            }

            player.velocity.y = std::min(player.velocity.y + Gravity * dt, 1200.0f);

            UpdateMovingPlatforms(dt);
            const float previousBottom = player.body.y + player.body.h;
            MovePlayerX(player.velocity.x * dt);
            MovePlayerY(player.velocity.y * dt);
            ResolveMovingPlatformLanding(previousBottom);
            UpdateEnemies(dt);
            CollectCoins();
            TouchCheckpoints();
            CheckHazards();

            if (exitRect.w > 0.0f && AllCoinsCollected() && Intersects(player.body, exitRect))
            {
                AdvanceRound();
            }

            if (player.body.y > height * TileSize + 400.0f)
            {
                DamagePlayer();
            }

            camera.x = player.body.x + player.body.w * 0.5f - WindowWidth * 0.5f;
            camera.y = player.body.y + player.body.h * 0.5f - WindowHeight * 0.55f;
            camera.x = std::max(0.0f, std::min(camera.x, std::max(0.0f, width * TileSize - WindowWidth)));
            camera.y = std::max(0.0f, std::min(camera.y, std::max(0.0f, height * TileSize - WindowHeight)));
        }

        void Render(ID2D1HwndRenderTarget* target, IDWriteTextFormat* textFormat, ID2D1SolidColorBrush* brush)
        {
            target->Clear(D2D1::ColorF(0x101820));
            DrawBackground(target, brush);
            DrawTiles(target, brush);
            DrawSpikes(target, brush);
            DrawMovingPlatforms(target, brush);
            DrawExit(target, brush);
            DrawCheckpoints(target, brush);
            DrawCoins(target, brush);
            DrawEnemies(target, brush);
            DrawJumpParticles(target, brush);
            DrawPlayer(target, brush);
            DrawHud(target, textFormat, brush);
            DrawStoryBanner(target, textFormat, brush);
        }

    private:
        enum class State
        {
            Menu,
            Playing,
            GameOver,
            Won
        };

        std::vector<std::string> rows;
        std::vector<std::string> firstRoundRows;
        std::vector<Coin> coins;
        std::vector<Enemy> enemies;
        std::vector<Rect> spikes;
        std::vector<Checkpoint> checkpoints;
        std::vector<MovingPlatform> movingPlatforms;
        std::vector<JumpParticle> jumpParticles;
        Player player;
        Rect exitRect;
        Vec2 initialSpawn;
        Vec2 checkpointSpawn;
        Vec2 camera;
        int width = 0;
        int height = 0;
        int currentRound = 0;
        float elapsed = 0.0f;
        float roundTime = 0.0f;
        float totalRunTime = 0.0f;
        float finalCompletionTime = -1.0f;
        float jumpAnimTimer = 0.0f;
        float storyBannerTimer = 0.0f;
        float bestRoundTimes[3] = { -1.0f, -1.0f, -1.0f };
        float bestGameTime = -1.0f;
        int selectedPlayerColor = 0;
        State state = State::Menu;

        float RoundEnemySpeed() const
        {
            return EnemySpeed + currentRound * 45.0f;
        }

        Rect StartButtonRect() const
        {
            return { WindowWidth * 0.5f - 150.0f, 470.0f, 300.0f, 72.0f };
        }

        std::vector<UINT32> PlayerPalette() const
        {
            return { 0x38BDF8, 0xF97316, 0x22C55E, 0xE879F9, 0xFACC15 };
        }

        std::vector<Rect> ColorSwatches() const
        {
            std::vector<Rect> swatches;
            const float startX = WindowWidth * 0.5f - 190.0f;
            for (int i = 0; i < 5; ++i)
            {
                swatches.push_back({ startX + i * 82.0f, 360.0f, 54.0f, 54.0f });
            }
            return swatches;
        }

        std::wstring BestScorePath() const
        {
            return L"best_time.txt";
        }

        std::wstring StoryIntro() const
        {
            return L"The Sky Core was shattered, and the floating city is collapsing. "
                L"Collect every energy coin, reopen each gate, and rebuild the core before the storm reaches the heart tower.";
        }

        std::wstring RoundStoryTitle(int index) const
        {
            static const wchar_t* titles[] = {
                L"District One: Broken Rooflines",
                L"District Two: Overgrown Lift Shafts",
                L"District Three: The Heart Tower"
            };
            return titles[std::clamp(index, 0, RoundCount() - 1)];
        }

        std::wstring RoundStoryText(int index) const
        {
            static const wchar_t* texts[] = {
                L"The first fragments are scattered across the outer blocks. Restore power to the first gate.",
                L"The storm has twisted the transport shafts. Stay moving and recover the second charge.",
                L"The final chamber is unstable. One last route remains before the city goes dark."
            };
            return texts[std::clamp(index, 0, RoundCount() - 1)];
        }

        std::wstring EndingStory() const
        {
            return L"The Sky Core is whole again. Light returns to the city, the machines restart, "
                L"and the last storm front breaks apart above the tower.";
        }

        void LoadBestScore()
        {
            std::ifstream file(BestScorePath());
            float loadedTime = -1.0f;
            if (file >> loadedTime && loadedTime > 0.0f)
            {
                bestGameTime = loadedTime;
            }
        }

        void SaveBestScore() const
        {
            if (bestGameTime <= 0.0f)
            {
                return;
            }

            std::ofstream file(BestScorePath(), std::ios::trunc);
            if (!file)
            {
                return;
            }

            file << std::fixed << std::setprecision(2) << bestGameTime;
        }

        void TriggerJumpEffects()
        {
            jumpAnimTimer = 0.22f;

            const float centerX = player.body.x + player.body.w * 0.5f;
            const float baseY = player.body.y + player.body.h - 4.0f;
            for (int i = 0; i < 8; ++i)
            {
                const float spread = static_cast<float>(i - 3) / 3.5f;
                jumpParticles.push_back({
                    { centerX + spread * 12.0f, baseY },
                    { spread * 95.0f, -120.0f - std::abs(spread) * 70.0f },
                    0.34f,
                    0.34f
                });
            }

            PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
        }

        void UpdateJumpParticles(float dt)
        {
            for (auto& particle : jumpParticles)
            {
                particle.life -= dt;
                particle.pos.x += particle.velocity.x * dt;
                particle.pos.y += particle.velocity.y * dt;
                particle.velocity.y += 520.0f * dt;
            }

            jumpParticles.erase(std::remove_if(jumpParticles.begin(), jumpParticles.end(),
                [](const JumpParticle& particle)
                {
                    return particle.life <= 0.0f;
                }), jumpParticles.end());
        }

        RoundStyle Style() const
        {
            static const RoundStyle styles[] = {
                { 0x14213D, 0x1F6F8B, 0x0B1020, 0x334155, 0x94A3B8, 0xA855F7, 0xF0ABFC, 0xEF4444 },
                { 0x17351F, 0x38A169, 0x07130A, 0x365314, 0xA3E635, 0x14B8A6, 0x99F6E4, 0xF97316 },
                { 0x2A102D, 0xE879F9, 0x100515, 0x4C1D95, 0xC4B5FD, 0xF43F5E, 0xFDA4AF, 0xFACC15 }
            };

            return styles[std::clamp(currentRound, 0, RoundCount() - 1)];
        }

        std::vector<std::string> BuiltInRound(int index) const
        {
            if (index == 1)
            {
                return {
                    "................................................................................",
                    "................................................................................",
                    "................................................................................",
                    ".................................C..............................................",
                    "............................########...........M................................",
                    "..............C........................................C.......................",
                    "..........########.................E...............########.....................",
                    "..................................#####.........................................",
                    "....P..................C.........M..........................^.........X..........",
                    "#########..........########................K.................#####...###########",
                    ".........................E.............########.................................",
                    "......................#####........^^^...............C........E................",
                    ".............C.................................############..#####..............",
                    ".........########.............#####.............................................",
                    ".................................C........M.........#####......................",
                    ".....K.......................#########.........E...............................",
                    "###########........E..........................#####.............#####..........",
                    "########################....############....############....###################",
                    "########################....############....############....###################",
                    "########################....############....############....###################"
                };
            }

            if (index == 2)
            {
                return {
                    "................................................................................",
                    "................................................................................",
                    ".............................................................C..................",
                    ".........................................................########...............",
                    "........................C......................................................",
                    "....................########...........M..........E............................",
                    "........C.............................C........#####...............C...........",
                    "....########.....................########.....................########..........",
                    "...................E..........................................................",
                    "....P............#####....M..........K...................E............^^.X.....",
                    "#########........................########..............#####..........##########",
                    "......................C........................................................",
                    "..................########...............E........C...........M................",
                    "......................................#####....########........E...............",
                    "..........K..................................................#####............",
                    "......########....^^^...E...........######...........######....................",
                    "......................#####.......................................######.......",
                    "###############....############....############....############....############",
                    "###############....############....############....############....############",
                    "###############....############....############....############....############"
                };
            }

            return {
                "................................................................................",
                "................................................................................",
                "........................C.......................................................",
                "....................########....................................................",
                "..........C.....................................................X..............",
                ".....P...#####....................K..........................########..........",
                "########...........E............#####.............C............................",
                "..................#####.......................########..........................",
                "##########################....##############################....................",
                "##########################....##############################...................."
            };
        }

        static bool Down(const bool* keys, int key)
        {
            return keys[key];
        }

        static bool Pressed(const bool* keys, int key)
        {
            return keys[key] && !previousKeys[key];
        }

        static bool previousKeys[256];

        bool IsSolid(int tileX, int tileY) const
        {
            if (tileY < 0 || tileY >= height || tileX < 0 || tileX >= width)
            {
                return false;
            }

            if (tileX >= static_cast<int>(rows[tileY].size()))
            {
                return false;
            }

            return rows[tileY][tileX] == '#';
        }

        std::vector<Rect> NearbySolidTiles(const Rect& area) const
        {
            std::vector<Rect> solids;
            const int left = static_cast<int>(std::floor(area.x / TileSize)) - 1;
            const int right = static_cast<int>(std::floor((area.x + area.w) / TileSize)) + 1;
            const int top = static_cast<int>(std::floor(area.y / TileSize)) - 1;
            const int bottom = static_cast<int>(std::floor((area.y + area.h) / TileSize)) + 1;

            for (int y = top; y <= bottom; ++y)
            {
                for (int x = left; x <= right; ++x)
                {
                    if (IsSolid(x, y))
                    {
                        solids.push_back({ x * TileSize, y * TileSize, TileSize, TileSize });
                    }
                }
            }

            return solids;
        }

        static bool OverlapsHorizontally(const Rect& a, const Rect& b, float inset = 0.0f)
        {
            return a.x + inset < b.x + b.w && a.x + a.w - inset > b.x;
        }

        bool IsStandingOnPlatform(const MovingPlatform& platform) const
        {
            const float feet = player.body.y + player.body.h;
            return OverlapsHorizontally(player.body, platform.body, 4.0f) &&
                std::fabs(feet - platform.body.y) <= 6.0f &&
                player.velocity.y >= -40.0f;
        }

        void ResolveSolidCarry(float amount)
        {
            for (const Rect& tile : NearbySolidTiles(player.body))
            {
                if (!Intersects(player.body, tile))
                {
                    continue;
                }

                if (amount > 0.0f)
                {
                    player.body.x = tile.x - player.body.w;
                }
                else if (amount < 0.0f)
                {
                    player.body.x = tile.x + tile.w;
                }
            }
        }

        void MovePlayerX(float amount)
        {
            player.body.x += amount;
            for (const Rect& tile : NearbySolidTiles(player.body))
            {
                if (!Intersects(player.body, tile))
                {
                    continue;
                }

                if (amount > 0.0f)
                {
                    player.body.x = tile.x - player.body.w;
                }
                else if (amount < 0.0f)
                {
                    player.body.x = tile.x + tile.w;
                }
                player.velocity.x = 0.0f;
            }
        }

        void MovePlayerY(float amount)
        {
            player.body.y += amount;
            player.onGround = false;

            for (const Rect& tile : NearbySolidTiles(player.body))
            {
                if (!Intersects(player.body, tile))
                {
                    continue;
                }

                if (amount > 0.0f)
                {
                    player.body.y = tile.y - player.body.h;
                    player.onGround = true;
                    player.jumpsUsed = 0;
                }
                else if (amount < 0.0f)
                {
                    player.body.y = tile.y + tile.h;
                }
                player.velocity.y = 0.0f;
            }
        }

        void UpdateMovingPlatforms(float dt)
        {
            for (auto& platform : movingPlatforms)
            {
                const bool carryingPlayer = IsStandingOnPlatform(platform);
                const float oldX = platform.body.x;
                platform.body.x += platform.vx * dt;

                if (platform.body.x < platform.minX)
                {
                    platform.body.x = platform.minX;
                    platform.vx = std::abs(platform.vx);
                }
                else if (platform.body.x > platform.maxX)
                {
                    platform.body.x = platform.maxX;
                    platform.vx = -std::abs(platform.vx);
                }

                const float movedX = platform.body.x - oldX;
                if (carryingPlayer && std::fabs(movedX) > 0.0f)
                {
                    player.body.x += movedX;
                    ResolveSolidCarry(movedX);
                }
            }
        }

        void ResolveMovingPlatformLanding(float previousBottom)
        {
            if (player.velocity.y < 0.0f)
            {
                return;
            }

            for (const auto& platform : movingPlatforms)
            {
                const float currentBottom = player.body.y + player.body.h;
                if (!OverlapsHorizontally(player.body, platform.body, 4.0f))
                {
                    continue;
                }

                if (previousBottom <= platform.body.y + 8.0f && currentBottom >= platform.body.y)
                {
                    player.body.y = platform.body.y - player.body.h;
                    player.velocity.y = 0.0f;
                    player.onGround = true;
                    player.jumpsUsed = 0;
                    break;
                }
            }
        }

        void UpdateEnemies(float dt)
        {
            for (auto& enemy : enemies)
            {
                if (!enemy.alive)
                {
                    continue;
                }

                enemy.body.x += enemy.vx * dt;
                bool hitWall = false;
                for (const Rect& tile : NearbySolidTiles(enemy.body))
                {
                    if (Intersects(enemy.body, tile))
                    {
                        if (enemy.vx > 0.0f)
                        {
                            enemy.body.x = tile.x - enemy.body.w;
                        }
                        else
                        {
                            enemy.body.x = tile.x + tile.w;
                        }
                        hitWall = true;
                    }
                }

                const float probeX = enemy.vx > 0.0f ? enemy.body.x + enemy.body.w + 2.0f : enemy.body.x - 2.0f;
                const float probeY = enemy.body.y + enemy.body.h + 4.0f;
                const bool groundAhead = IsSolid(static_cast<int>(probeX / TileSize), static_cast<int>(probeY / TileSize));
                if (hitWall || !groundAhead)
                {
                    enemy.vx *= -1.0f;
                }
            }
        }

        void CollectCoins()
        {
            for (auto& coin : coins)
            {
                if (coin.collected)
                {
                    continue;
                }

                Rect coinRect = { coin.pos.x - 14.0f, coin.pos.y - 14.0f, 28.0f, 28.0f };
                if (Intersects(player.body, coinRect))
                {
                    coin.collected = true;
                    ++player.coins;
                }
            }
        }

        void TouchCheckpoints()
        {
            for (auto& checkpoint : checkpoints)
            {
                Rect area = { checkpoint.pos.x - 6.0f, checkpoint.pos.y, 34.0f, 48.0f };
                if (Intersects(player.body, area))
                {
                    for (auto& other : checkpoints)
                    {
                        other.active = false;
                    }
                    checkpoint.active = true;
                    checkpointSpawn = { checkpoint.pos.x, checkpoint.pos.y };
                }
            }
        }

        void CheckHazards()
        {
            for (const Rect& spike : spikes)
            {
                if (Intersects(player.body, spike))
                {
                    DamagePlayer();
                    return;
                }
            }

            for (auto& enemy : enemies)
            {
                if (!enemy.alive || !Intersects(player.body, enemy.body))
                {
                    continue;
                }

                const bool stomped = player.velocity.y > 120.0f && player.body.y + player.body.h - enemy.body.y < 18.0f;
                if (stomped)
                {
                    enemy.alive = false;
                    player.velocity.y = JumpVelocity * 0.45f;
                }
                else
                {
                    DamagePlayer();
                }
            }
        }

        void DamagePlayer()
        {
            --player.lives;
            if (player.lives <= 0)
            {
                state = State::GameOver;
                return;
            }

            ResetRun();
        }

        void AdvanceRound()
        {
            RecordBestTime();
            if (currentRound + 1 >= RoundCount())
            {
                finalCompletionTime = totalRunTime;
                if (bestGameTime < 0.0f || finalCompletionTime < bestGameTime)
                {
                    bestGameTime = finalCompletionTime;
                    SaveBestScore();
                }
                state = State::Won;
                return;
            }

            LoadRound(currentRound + 1);
        }

        bool AllCoinsCollected() const
        {
            return player.coins >= static_cast<int>(coins.size());
        }

        void RecordBestTime()
        {
            const int index = std::clamp(currentRound, 0, RoundCount() - 1);
            if (bestRoundTimes[index] < 0.0f || roundTime < bestRoundTimes[index])
            {
                bestRoundTimes[index] = roundTime;
            }
        }

        void DrawBackground(ID2D1HwndRenderTarget* target, ID2D1SolidColorBrush* brush)
        {
            const RoundStyle style = Style();
            brush->SetColor(D2D1::ColorF(style.sky));
            target->FillRectangle(D2D1::RectF(0, 0, WindowWidth, WindowHeight), brush);

            brush->SetColor(D2D1::ColorF(style.cloud, 0.35f));
            for (int i = 0; i < 18; ++i)
            {
                const float x = std::fmod(i * 173.0f - camera.x * 0.18f, WindowWidth + 160.0f) - 80.0f;
                const float y = 80.0f + std::sin(i * 1.7f) * 36.0f;
                target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 34.0f, 10.0f), brush);
            }

            brush->SetColor(D2D1::ColorF(style.ground));
            target->FillRectangle(D2D1::RectF(0, WindowHeight - 52.0f, WindowWidth, WindowHeight), brush);
        }

        void DrawMenu(ID2D1HwndRenderTarget* target, IDWriteTextFormat* textFormat, ID2D1SolidColorBrush* brush)
        {
            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            brush->SetColor(D2D1::ColorF(0x0B1020));
            target->FillRectangle(D2D1::RectF(0, 0, WindowWidth, WindowHeight), brush);

            brush->SetColor(D2D1::ColorF(0x1D4ED8, 0.25f));
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(260.0f, 180.0f), 220.0f, 120.0f), brush);
            brush->SetColor(D2D1::ColorF(0xF97316, 0.22f));
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(980.0f, 540.0f), 260.0f, 140.0f), brush);

            brush->SetColor(D2D1::ColorF(0xF8FAFC));
            const std::wstring title = L"PlatformerDX";
            target->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), textFormat,
                D2D1::RectF(390.0f, 120.0f, 920.0f, 180.0f), brush);

            const std::wstring subtitle = L"Choose your character color and start the game";
            target->DrawTextW(subtitle.c_str(), static_cast<UINT32>(subtitle.size()), textFormat,
                D2D1::RectF(260.0f, 185.0f, 1020.0f, 235.0f), brush);

            const std::wstring introStory = StoryIntro();
            target->DrawTextW(introStory.c_str(), static_cast<UINT32>(introStory.size()), textFormat,
                D2D1::RectF(220.0f, 255.0f, 1060.0f, 335.0f), brush);

            std::wstringstream bestScoreText;
            bestScoreText.setf(std::ios::fixed);
            bestScoreText.precision(1);
            if (bestGameTime > 0.0f)
            {
                bestScoreText << L"Best total time: " << bestGameTime << L" seconds";
            }
            else
            {
                bestScoreText << L"Best total time: no score yet";
            }
            const std::wstring bestText = bestScoreText.str();
            target->DrawTextW(bestText.c_str(), static_cast<UINT32>(bestText.size()), textFormat,
                D2D1::RectF(280.0f, 225.0f, 1000.0f, 275.0f), brush);

            Rect preview = { WindowWidth * 0.5f - 30.0f, 250.0f, 60.0f, 80.0f };
            brush->SetColor(D2D1::ColorF(PlayerPalette()[selectedPlayerColor]));
            target->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(preview, {}), 8.0f, 8.0f), brush);
            brush->SetColor(D2D1::ColorF(0x0F172A));
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(preview.x + 40.0f, preview.y + 22.0f), 4.0f, 4.0f), brush);

            const auto palette = PlayerPalette();
            const auto swatches = ColorSwatches();
            for (int i = 0; i < static_cast<int>(swatches.size()); ++i)
            {
                brush->SetColor(D2D1::ColorF(palette[i]));
                target->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(swatches[i], {}), 8.0f, 8.0f), brush);
                brush->SetColor(i == selectedPlayerColor ? D2D1::ColorF(0xFFFFFF) : D2D1::ColorF(0x334155));
                target->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(swatches[i], {}), 8.0f, 8.0f), brush,
                    i == selectedPlayerColor ? 4.0f : 2.0f);
            }

            const Rect button = StartButtonRect();
            brush->SetColor(D2D1::ColorF(0x22C55E));
            target->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(button, {}), 12.0f, 12.0f), brush);
            brush->SetColor(D2D1::ColorF(0xDCFCE7));
            target->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(button, {}), 12.0f, 12.0f), brush, 3.0f);

            const std::wstring buttonText = L"Start Game";
            brush->SetColor(D2D1::ColorF(0x052E16));
            target->DrawTextW(buttonText.c_str(), static_cast<UINT32>(buttonText.size()), textFormat,
                D2D1::RectF(button.x, button.y + 16.0f, button.x + button.w, button.y + button.h), brush);
            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        void DrawStoryBanner(ID2D1HwndRenderTarget* target, IDWriteTextFormat* textFormat, ID2D1SolidColorBrush* brush)
        {
            if (storyBannerTimer <= 0.0f || state != State::Playing)
            {
                return;
            }

            const float alpha = std::min(1.0f, storyBannerTimer / 0.8f);
            brush->SetColor(D2D1::ColorF(0x020617, 0.72f * alpha));
            target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(140.0f, 110.0f, 1140.0f, 220.0f), 16.0f, 16.0f), brush);

            brush->SetColor(D2D1::ColorF(0xE2E8F0, alpha));
            const std::wstring title = RoundStoryTitle(currentRound);
            target->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), textFormat,
                D2D1::RectF(180.0f, 130.0f, 1100.0f, 165.0f), brush);

            const std::wstring text = RoundStoryText(currentRound);
            target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), textFormat,
                D2D1::RectF(180.0f, 170.0f, 1100.0f, 215.0f), brush);
        }

        void DrawTiles(ID2D1HwndRenderTarget* target, ID2D1SolidColorBrush* brush)
        {
            const RoundStyle style = Style();
            const int left = std::max(0, static_cast<int>(camera.x / TileSize) - 1);
            const int right = std::min(width - 1, static_cast<int>((camera.x + WindowWidth) / TileSize) + 1);
            const int top = std::max(0, static_cast<int>(camera.y / TileSize) - 1);
            const int bottom = std::min(height - 1, static_cast<int>((camera.y + WindowHeight) / TileSize) + 1);

            for (int y = top; y <= bottom; ++y)
            {
                for (int x = left; x <= right; ++x)
                {
                    if (!IsSolid(x, y))
                    {
                        continue;
                    }

                    const Rect tile = { x * TileSize, y * TileSize, TileSize, TileSize };
                    brush->SetColor(D2D1::ColorF(style.tile));
                    target->FillRectangle(ToD2D(tile, camera), brush);

                    brush->SetColor(D2D1::ColorF(style.tileTop));
                    const Rect topEdge = { tile.x, tile.y, tile.w, 5.0f };
                    target->FillRectangle(ToD2D(topEdge, camera), brush);

                    brush->SetColor(D2D1::ColorF(0x111827));
                    target->DrawRectangle(ToD2D(tile, camera), brush, 1.0f);
                }
            }
        }

        void DrawSpikes(ID2D1HwndRenderTarget* target, ID2D1SolidColorBrush* brush)
        {
            for (const Rect& spike : spikes)
            {
                brush->SetColor(D2D1::ColorF(0x7C2D12));
                Rect base = { spike.x, spike.y + spike.h - 6.0f, spike.w, 6.0f };
                target->FillRectangle(ToD2D(base, camera), brush);

                brush->SetColor(D2D1::ColorF(0xF97316));
                const float segment = spike.w / 3.0f;
                for (int i = 0; i < 3; ++i)
                {
                    const float x0 = spike.x + i * segment - camera.x;
                    const float y0 = spike.y + spike.h - camera.y;
                    const float x1 = spike.x + (i + 0.5f) * segment - camera.x;
                    const float y1 = spike.y - camera.y;
                    const float x2 = spike.x + (i + 1.0f) * segment - camera.x;
                    target->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush, 2.0f);
                    target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y0), brush, 2.0f);
                    target->DrawLine(D2D1::Point2F(x2, y0), D2D1::Point2F(x0, y0), brush, 2.0f);
                }
            }
        }

        void DrawMovingPlatforms(ID2D1HwndRenderTarget* target, ID2D1SolidColorBrush* brush)
        {
            for (const auto& platform : movingPlatforms)
            {
                brush->SetColor(D2D1::ColorF(0xF59E0B));
                target->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(platform.body, camera), 5.0f, 5.0f), brush);
                brush->SetColor(D2D1::ColorF(0xFFFBEB));
                Rect stripe = { platform.body.x + 8.0f, platform.body.y + 4.0f, platform.body.w - 16.0f, 3.0f };
                target->FillRectangle(ToD2D(stripe, camera), brush);
            }
        }

        void DrawJumpParticles(ID2D1HwndRenderTarget* target, ID2D1SolidColorBrush* brush)
        {
            for (const auto& particle : jumpParticles)
            {
                const float alpha = std::max(0.0f, particle.life / particle.maxLife);
                brush->SetColor(D2D1::ColorF(0xE0F2FE, alpha));
                target->FillEllipse(
                    D2D1::Ellipse(
                        D2D1::Point2F(particle.pos.x - camera.x, particle.pos.y - camera.y),
                        4.0f + (1.0f - alpha) * 3.0f,
                        4.0f + (1.0f - alpha) * 3.0f),
                    brush);
            }
        }

        void DrawCoins(ID2D1HwndRenderTarget* target, ID2D1SolidColorBrush* brush)
        {
            for (const auto& coin : coins)
            {
                if (coin.collected)
                {
                    continue;
                }

                const float bob = std::sin(elapsed * 5.0f + coin.pos.x * 0.03f) * 4.0f;
                const D2D1_POINT_2F center = D2D1::Point2F(coin.pos.x - camera.x, coin.pos.y - camera.y + bob);
                brush->SetColor(D2D1::ColorF(0xFBBF24));
                target->FillEllipse(D2D1::Ellipse(center, 11.0f, 15.0f), brush);
                brush->SetColor(D2D1::ColorF(0xFEF3C7));
                target->DrawEllipse(D2D1::Ellipse(center, 11.0f, 15.0f), brush, 2.0f);
            }
        }

        void DrawCheckpoints(ID2D1HwndRenderTarget* target, ID2D1SolidColorBrush* brush)
        {
            for (const auto& checkpoint : checkpoints)
            {
                Rect pole = { checkpoint.pos.x, checkpoint.pos.y, 5.0f, 48.0f };
                brush->SetColor(D2D1::ColorF(0xE5E7EB));
                target->FillRectangle(ToD2D(pole, camera), brush);

                Rect flag = { checkpoint.pos.x + 5.0f, checkpoint.pos.y + 4.0f, 28.0f, 18.0f };
                brush->SetColor(checkpoint.active ? D2D1::ColorF(0x22C55E) : D2D1::ColorF(0x60A5FA));
                target->FillRectangle(ToD2D(flag, camera), brush);
            }
        }

        void DrawExit(ID2D1HwndRenderTarget* target, ID2D1SolidColorBrush* brush)
        {
            if (exitRect.w <= 0.0f)
            {
                return;
            }

            const auto rect = ToD2D(exitRect, camera);
            const RoundStyle style = Style();
            const bool unlocked = AllCoinsCollected();
            brush->SetColor(unlocked ? D2D1::ColorF(style.exit) : D2D1::ColorF(0x475569));
            target->FillRoundedRectangle(D2D1::RoundedRect(rect, 8.0f, 8.0f), brush);
            brush->SetColor(unlocked ? D2D1::ColorF(style.exitGlow) : D2D1::ColorF(0xCBD5E1));
            target->DrawRoundedRectangle(D2D1::RoundedRect(rect, 8.0f, 8.0f), brush, 3.0f);
        }

        void DrawEnemies(ID2D1HwndRenderTarget* target, ID2D1SolidColorBrush* brush)
        {
            for (const auto& enemy : enemies)
            {
                if (!enemy.alive)
                {
                    continue;
                }

                brush->SetColor(D2D1::ColorF(Style().enemy));
                target->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(enemy.body, camera), 5.0f, 5.0f), brush);
                brush->SetColor(D2D1::ColorF(0x111827));
                const float eyeX = enemy.vx > 0.0f ? enemy.body.x + 24.0f : enemy.body.x + 9.0f;
                target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(eyeX - camera.x, enemy.body.y + 14.0f - camera.y), 4.0f, 4.0f), brush);
            }
        }

        void DrawPlayer(ID2D1HwndRenderTarget* target, ID2D1SolidColorBrush* brush)
        {
            const auto palette = PlayerPalette();
            const UINT32 bodyColor = palette[selectedPlayerColor];
            Rect drawBody = player.body;
            if (!player.onGround)
            {
                drawBody.x += 2.5f;
                drawBody.w -= 5.0f;
                drawBody.h += 3.0f;
            }
            if (jumpAnimTimer > 0.0f)
            {
                const float stretch = jumpAnimTimer / 0.22f;
                drawBody.x -= 2.0f * stretch;
                drawBody.w += 4.0f * stretch;
                drawBody.h -= 5.0f * stretch;
                drawBody.y += 5.0f * stretch;
            }

            brush->SetColor(player.onGround ? D2D1::ColorF(bodyColor) : D2D1::ColorF(bodyColor, 0.82f));
            target->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(drawBody, camera), 6.0f, 6.0f), brush);

            if (!player.onGround)
            {
                const float sway = std::sin(elapsed * 18.0f) * 3.0f;
                brush->SetColor(D2D1::ColorF(bodyColor, 0.25f));
                target->DrawLine(
                    D2D1::Point2F(player.body.x - 6.0f - camera.x, player.body.y + 24.0f - camera.y),
                    D2D1::Point2F(player.body.x - 18.0f - camera.x, player.body.y + 12.0f + sway - camera.y),
                    brush, 4.0f);
                target->DrawLine(
                    D2D1::Point2F(player.body.x + player.body.w + 6.0f - camera.x, player.body.y + 24.0f - camera.y),
                    D2D1::Point2F(player.body.x + player.body.w + 18.0f - camera.x, player.body.y + 12.0f - sway - camera.y),
                    brush, 4.0f);
            }

            brush->SetColor(D2D1::ColorF(0x0F172A));
            const float eyeOffset = player.velocity.x >= 0.0f ? 22.0f : 9.0f;
            const float eyeY = !player.onGround ? 12.0f : 14.0f;
            target->FillEllipse(D2D1::Ellipse(
                D2D1::Point2F(drawBody.x + eyeOffset - camera.x, drawBody.y + eyeY - camera.y), 4.0f, 4.0f), brush);
        }

        void DrawHud(ID2D1HwndRenderTarget* target, IDWriteTextFormat* textFormat, ID2D1SolidColorBrush* brush)
        {
            if (state == State::Menu)
            {
                DrawMenu(target, textFormat, brush);
                return;
            }

            std::wstringstream hud;
            hud << L"Round: " << currentRound + 1 << L"/" << RoundCount()
                << L"    Lives: " << player.lives
                << L"    Coins: " << player.coins << L"/" << coins.size()
                << L"    Exit: " << (AllCoinsCollected() ? L"Unlocked" : L"Locked");

            brush->SetColor(D2D1::ColorF(0xF8FAFC));
            const std::wstring hudText = hud.str();
            target->DrawTextW(hudText.c_str(), static_cast<UINT32>(hudText.size()), textFormat,
                D2D1::RectF(22.0f, 18.0f, 980.0f, 64.0f), brush);

            std::wstringstream timer;
            timer.setf(std::ios::fixed);
            timer.precision(1);
            timer << L"Time: " << roundTime << L"s";
            timer << L"    Total: " << totalRunTime << L"s";
            if (bestRoundTimes[currentRound] >= 0.0f)
            {
                timer << L"    Best: " << bestRoundTimes[currentRound] << L"s";
            }

            const std::wstring timerText = timer.str();
            target->DrawTextW(timerText.c_str(), static_cast<UINT32>(timerText.size()), textFormat,
                D2D1::RectF(22.0f, 54.0f, 720.0f, 96.0f), brush);

            if (state == State::Won || state == State::GameOver)
            {
                brush->SetColor(D2D1::ColorF(0x000000, 0.55f));
                target->FillRectangle(D2D1::RectF(0, 0, WindowWidth, WindowHeight), brush);

                std::wstringstream message;
                if (state == State::Won)
                {
                    message.setf(std::ios::fixed);
                    message.precision(1);
                    message << L"You finished all rounds in " << finalCompletionTime
                        << L"s. Best score: ";
                    if (bestGameTime > 0.0f)
                    {
                        message << bestGameTime << L"s";
                    }
                    else
                    {
                        message << L"none";
                    }
                    message << L". " << EndingStory() << L" Press R to restart.";
                }
                else
                {
                    message << L"Game Over. Press R to restart.";
                }
                brush->SetColor(D2D1::ColorF(0xFFFFFF));
                const std::wstring messageText = message.str();
                target->DrawTextW(messageText.c_str(), static_cast<UINT32>(messageText.size()), textFormat,
                    D2D1::RectF(330.0f, 320.0f, 980.0f, 390.0f), brush);
            }
        }

    public:
        static void CommitInputFrame(const bool* keys)
        {
            for (int i = 0; i < 256; ++i)
            {
                previousKeys[i] = keys[i];
            }
        }
    };

    bool Game::previousKeys[256] = {};

    class PlatformerApp
    {
    public:
        HRESULT Initialize(HINSTANCE instance)
        {
            HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
            if (FAILED(hr))
            {
                return hr;
            }

            hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf()));
            if (FAILED(hr))
            {
                return hr;
            }

            hr = writeFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 28.0f, L"en-us", textFormat.GetAddressOf());
            if (FAILED(hr))
            {
                return hr;
            }

            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

            WNDCLASSEX wc = {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = L"PlatformerDXWindowClass";
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

            RegisterClassEx(&wc);

            RECT rc = { 0, 0, static_cast<LONG>(WindowWidth), static_cast<LONG>(WindowHeight) };
            AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

            hwnd = CreateWindowEx(0, wc.lpszClassName, L"PlatformerDX - Direct2D Platformer",
                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                nullptr, nullptr, instance, this);

            if (!hwnd)
            {
                return E_FAIL;
            }

            game.LoadLevel(L"levels/level1.txt");
            ShowWindow(hwnd, SW_SHOWNORMAL);
            return S_OK;
        }

        int Run()
        {
            MSG msg = {};
            auto previous = std::chrono::high_resolution_clock::now();
            float accumulator = 0.0f;

            while (msg.message != WM_QUIT)
            {
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }

                auto now = std::chrono::high_resolution_clock::now();
                float frameTime = std::chrono::duration<float>(now - previous).count();
                previous = now;
                frameTime = std::min(frameTime, 0.25f);
                accumulator += frameTime;

                while (accumulator >= FixedDt)
                {
                    game.Update(FixedDt, keys);
                    Game::CommitInputFrame(keys);
                    accumulator -= FixedDt;
                }

                Render();
            }

            return static_cast<int>(msg.wParam);
        }

    private:
        HWND hwnd = nullptr;
        bool keys[256] = {};
        Game game;
        ComPtr<ID2D1Factory> factory;
        ComPtr<IDWriteFactory> writeFactory;
        ComPtr<IDWriteTextFormat> textFormat;
        ComPtr<ID2D1HwndRenderTarget> renderTarget;
        ComPtr<ID2D1SolidColorBrush> brush;

        HRESULT CreateDeviceResources()
        {
            if (renderTarget)
            {
                return S_OK;
            }

            RECT rc;
            GetClientRect(hwnd, &rc);
            const D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

            HRESULT hr = factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(hwnd, size), renderTarget.GetAddressOf());
            if (FAILED(hr))
            {
                return hr;
            }

            hr = renderTarget->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), brush.GetAddressOf());
            return hr;
        }

        void DiscardDeviceResources()
        {
            brush.Reset();
            renderTarget.Reset();
        }

        void Render()
        {
            if (FAILED(CreateDeviceResources()))
            {
                return;
            }

            renderTarget->BeginDraw();
            game.Render(renderTarget.Get(), textFormat.Get(), brush.Get());
            const HRESULT hr = renderTarget->EndDraw();

            if (hr == D2DERR_RECREATE_TARGET)
            {
                DiscardDeviceResources();
            }
        }

        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
            case WM_SIZE:
                if (renderTarget)
                {
                    renderTarget->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
                }
                return 0;
            case WM_KEYDOWN:
                if (wParam < 256)
                {
                    keys[wParam] = true;
                }
                if (wParam == VK_ESCAPE)
                {
                    PostQuitMessage(0);
                }
                return 0;
            case WM_KEYUP:
                if (wParam < 256)
                {
                    keys[wParam] = false;
                }
                return 0;
            case WM_LBUTTONDOWN:
                game.HandleClick(static_cast<float>(GET_X_LPARAM(lParam)),
                    static_cast<float>(GET_Y_LPARAM(lParam)));
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProc(hwnd, message, wParam, lParam);
            }
        }

        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            PlatformerApp* app = nullptr;
            if (message == WM_NCCREATE)
            {
                auto create = reinterpret_cast<CREATESTRUCT*>(lParam);
                app = reinterpret_cast<PlatformerApp*>(create->lpCreateParams);
                SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
                app->hwnd = hwnd;
            }
            else
            {
                app = reinterpret_cast<PlatformerApp*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
            }

            if (app)
            {
                return app->HandleMessage(message, wParam, lParam);
            }

            return DefWindowProc(hwnd, message, wParam, lParam);
        }
    };
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    PlatformerApp app;
    if (FAILED(app.Initialize(instance)))
    {
        MessageBox(nullptr, L"Failed to initialize PlatformerDX.", L"PlatformerDX", MB_ICONERROR);
        return -1;
    }

    return app.Run();
}
