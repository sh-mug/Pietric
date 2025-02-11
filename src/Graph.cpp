#include "Graph.h"
#include <queue>
#include <iostream>
#include <algorithm>
#include <map>

// Helper: returns true if (r, c) is within the grid bounds.
static bool inBounds(int r, int c, int rows, int cols) {
    return (r >= 0 && r < rows && c >= 0 && c < cols);
}

Graph::Graph() {
}

void Graph::computeBlocks(const std::vector<std::vector<PietColor>> &grid) {
    int rows = grid.size();
    if (rows == 0) return;
    int cols = grid[0].size();
    std::vector<std::vector<bool>> visited(rows, std::vector<bool>(cols, false));
    int blockId = 0;
    const int dr[4] = { -1, 1, 0, 0 };
    const int dc[4] = { 0, 0, -1, 1 };

    blocks.clear();
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if (visited[r][c]) continue;
            PietColor color = grid[r][c];
            // Create a new block.
            Block block;
            block.id = blockId++;
            block.color = color;
            block.size = 0;
            std::queue<std::pair<int,int>> q;
            q.push({r, c});
            visited[r][c] = true;
            while (!q.empty()) {
                auto [cr, cc] = q.front();
                q.pop();
                block.cells.push_back({cr, cc});
                block.size++;
                for (int i = 0; i < 4; ++i) {
                    int nr = cr + dr[i], nc = cc + dc[i];
                    if (inBounds(nr, nc, rows, cols) && !visited[nr][nc] &&
                        grid[nr][nc] == color) {
                        visited[nr][nc] = true;
                        q.push({nr, nc});
                    }
                }
            }
            blocks.push_back(block);
        }
    }
}

Command Graph::getCommand(PietColor from, PietColor to) {
    // If either block is white or black or the colors are the same, no command is executed.
    if (from == PietColor::White || from == PietColor::Black ||
        to == PietColor::White || to == PietColor::Black ||
        from == to)
        return Command::None;

    auto getHueLight = [](PietColor color, int &hue, int &lightness) -> bool {
        switch (color) {
            case PietColor::LightRed:     hue = 0; lightness = 0; return true;
            case PietColor::LightYellow:  hue = 1; lightness = 0; return true;
            case PietColor::LightGreen:   hue = 2; lightness = 0; return true;
            case PietColor::LightCyan:    hue = 3; lightness = 0; return true;
            case PietColor::LightBlue:    hue = 4; lightness = 0; return true;
            case PietColor::LightMagenta: hue = 5; lightness = 0; return true;
            case PietColor::Red:          hue = 0; lightness = 1; return true;
            case PietColor::Yellow:       hue = 1; lightness = 1; return true;
            case PietColor::Green:        hue = 2; lightness = 1; return true;
            case PietColor::Cyan:         hue = 3; lightness = 1; return true;
            case PietColor::Blue:         hue = 4; lightness = 1; return true;
            case PietColor::Magenta:      hue = 5; lightness = 1; return true;
            case PietColor::DarkRed:      hue = 0; lightness = 2; return true;
            case PietColor::DarkYellow:   hue = 1; lightness = 2; return true;
            case PietColor::DarkGreen:    hue = 2; lightness = 2; return true;
            case PietColor::DarkCyan:     hue = 3; lightness = 2; return true;
            case PietColor::DarkBlue:     hue = 4; lightness = 2; return true;
            case PietColor::DarkMagenta:  hue = 5; lightness = 2; return true;
            default: return false;
        }
    };

    int fromHue, fromLight;
    int toHue, toLight;
    if (!getHueLight(from, fromHue, fromLight) || !getHueLight(to, toHue, toLight))
        return Command::None;

    int hueDiff = (toHue - fromHue + 6) % 6;
    int lightDiff = (toLight - fromLight + 3) % 3;
    if (hueDiff == 0 && lightDiff == 0)
        return Command::None;

    if (lightDiff == 0) {
        switch (hueDiff) {
            case 1: return Command::Add;
            case 2: return Command::Divide;
            case 3: return Command::Greater;
            case 4: return Command::Duplicate;
            case 5: return Command::InputChar;
            default: break;
        }
    } else if (lightDiff == 1) {
        switch (hueDiff) {
            case 0: return Command::Push;
            case 1: return Command::Subtract;
            case 2: return Command::Modulo;
            case 3: return Command::Pointer;
            case 4: return Command::Roll;
            case 5: return Command::OutputNum;
            default: break;
        }
    } else if (lightDiff == 2) {
        switch (hueDiff) {
            case 0: return Command::Pop;
            case 1: return Command::Multiply;
            case 2: return Command::Not;
            case 3: return Command::Switch;
            case 4: return Command::InputNum;
            case 5: return Command::OutputChar;
            default: break;
        }
    }
    return Command::None;
}

// --- Helper: getExitCodel ---
// Given a block, select the exit codel (the codel in the block that is farthest in the DP direction,
// breaking ties using the CC).
std::pair<int,int> Graph::getExitCodel(const Block &block, Direction dp, CodelChooser cc,
                                         const std::vector<std::vector<PietColor>> &grid) {
    // Initialize with the first cell in the block.
    std::pair<int,int> best = block.cells[0];
    for (const auto &cell : block.cells) {
        int r = cell.first, c = cell.second;
        int best_r = best.first, best_c = best.second;
        switch (dp) {
            case Direction::Right:
                if (c > best_c ||
                    (c == best_c && ((cc == CodelChooser::Left && r < best_r) ||
                                      (cc == CodelChooser::Right && r > best_r))))
                    best = cell;
                break;
            case Direction::Left:
                if (c < best_c ||
                    (c == best_c && ((cc == CodelChooser::Left && r > best_r) ||
                                      (cc == CodelChooser::Right && r < best_r))))
                    best = cell;
                break;
            case Direction::Down:
                if (r > best_r ||
                    (r == best_r && ((cc == CodelChooser::Left && c < best_c) ||
                                      (cc == CodelChooser::Right && c > best_c))))
                    best = cell;
                break;
            case Direction::Up:
                if (r < best_r ||
                    (r == best_r && ((cc == CodelChooser::Left && c > best_c) ||
                                      (cc == CodelChooser::Right && c < best_c))))
                    best = cell;
                break;
        }
    }
    return best;
}

// --- Helper: getNextCodel ---
// Simply add the appropriate delta to move one codel in the given DP.
std::pair<int,int> Graph::getNextCodel(const std::pair<int,int>& coord, Direction dp) {
    int r = coord.first, c = coord.second;
    switch (dp) {
        case Direction::Right: return {r, c + 1};
        case Direction::Down:  return {r + 1, c};
        case Direction::Left:  return {r, c - 1};
        case Direction::Up:    return {r - 1, c};
    }
    return coord; // Should not reach here.
}

// --- Helper: findBlockId ---
// Look through the computed blocks to find one that contains (r,c).
int Graph::findBlockId(int r, int c) const {
    for (const auto &b : blocks) {
        for (const auto &cell : b.cells) {
            if (cell.first == r && cell.second == c)
                return b.id;
        }
    }
    return -1;
}

// --- Helper: rotateDP ---
// Rotate the direction pointer clockwise by 'steps' (each step is 90 degrees).
Direction Graph::rotateDP(Direction dp, int steps) {
    int d = static_cast<int>(dp);
    d = (d + steps) % 4;
    return static_cast<Direction>(d);
}

// --- Helper: toggleCC ---
// Toggle the codel chooser.
CodelChooser Graph::toggleCC(CodelChooser cc) {
    return (cc == CodelChooser::Left ? CodelChooser::Right : CodelChooser::Left);
}

// --- BuildGraph ---
// Here we simulate Piet movement from one block to the next following the DP/CC rules.
// We use a worklist to traverse reachable states. A state is a triple: (blockId, dp, cc).
void Graph::buildGraph(const std::vector<std::vector<PietColor>> &grid) {
    nodes.clear();
    computeBlocks(grid);
    if (blocks.empty()) return;

    // If the color of the top–left codel is black or white, we cannot start.
    if (grid[0][0] == PietColor::Black || grid[0][0] == PietColor::White) return;

    // Find the block that contains the top–left pixel (0,0).
    int initialBlockId = findBlockId(0, 0);
    if (initialBlockId < 0) return;

    // Worklist: store indices of nodes to process.
    std::vector<int> worklist;
    // Map key: (blockId, dp, cc) encoded as a tuple; value: node id.
    struct StateKey { int blockId; int dp; int cc; };
    auto keyHash = [](const StateKey &k) { return k.blockId * 100 + k.dp * 10 + k.cc; };
    std::vector<StateKey> stateKeys;
    std::map<int, int> stateMap; // keyHash -> node id (for simplicity; collisions are unlikely for our demo)

    auto makeKey = [&](int bid, Direction d, CodelChooser c) -> int {
        StateKey key{ bid, static_cast<int>(d), static_cast<int>(c) };
        return keyHash(key);
    };

    // Create initial state.
    GraphNode init;
    init.id = 0;
    init.blockId = initialBlockId;
    init.blockSize = blocks[initialBlockId].size;
    init.dp = Direction::Right;
    init.cc = CodelChooser::Left;
    nodes.push_back(init);
    worklist.push_back(0);
    stateMap[makeKey(initialBlockId, init.dp, init.cc)] = 0;

    int rows = grid.size();
    int cols = grid[0].size();

    // Process worklist.
    while (!worklist.empty()) {
        int curId = worklist.back();
        worklist.pop_back();
        GraphNode curState = nodes[curId];
        Block curBlock = blocks[curState.blockId];

        // --- Attempt to compute a valid exit ---
        bool foundExit = false;
        Direction trialDP = curState.dp;
        CodelChooser trialCC = curState.cc;
        std::pair<int,int> exitCoord, candidate;
        for (int attempt = 0; attempt < 8; ++attempt) {
            exitCoord = getExitCodel(curBlock, trialDP, trialCC, grid);
            candidate = getNextCodel(exitCoord, trialDP);
            int r = candidate.first, c = candidate.second;
            // Check bounds and whether candidate is not black.
            if (!inBounds(r, c, rows, cols) || grid[r][c] == PietColor::Black) {
                // No valid candidate this attempt.
                // Piet rule: first toggle CC on even attempts, then rotate DP on odd attempts.
                if (attempt % 2 == 0)
                    trialCC = toggleCC(trialCC);
                else
                    trialDP = rotateDP(trialDP, 1);
                continue;
            } else {
                foundExit = true;
                break;
            }
        }
        if (!foundExit) {
            // Terminal state; no valid exit.
            continue;
        }

        // If the candidate is white, follow the white region until reaching a colored block.
        while (inBounds(candidate.first, candidate.second, rows, cols) &&
               grid[candidate.first][candidate.second] == PietColor::White) {
            candidate = getNextCodel(candidate, trialDP);
        }
        // If out-of-bounds or black after following white, then terminate.
        if (!inBounds(candidate.first, candidate.second, rows, cols) ||
            grid[candidate.first][candidate.second] == PietColor::Black)
            continue;

        int targetBlockId = findBlockId(candidate.first, candidate.second);
        if (targetBlockId < 0)
            continue;

        // Compute the command from current block color to the target block's color.
        Command cmd = getCommand(curBlock.color, blocks[targetBlockId].color);

        // Determine possible new DP/CC outcomes.
        std::vector<std::pair<Direction, CodelChooser>> outcomes;
        if (cmd == Command::Pointer) {
            // Pointer command: the DP rotates; for demonstration we add all four possibilities.
            for (int i = 0; i < 4; i++) {
                outcomes.push_back({rotateDP(trialDP, i), trialCC});
            }
        } else if (cmd == Command::Switch) {
            // Switch command: the CC toggles (or not). Two possibilities.
            outcomes.push_back({trialDP, trialCC});
            outcomes.push_back({trialDP, toggleCC(trialCC)});
        } else {
            outcomes.push_back({trialDP, trialCC});
        }

        // For each outcome, create (or reuse) a new state and add an edge.
        for (const auto &outcome : outcomes) {
            Direction newDP = outcome.first;
            CodelChooser newCC = outcome.second;
            int key = makeKey(targetBlockId, newDP, newCC);
            int targetNodeId;
            if (stateMap.find(key) == stateMap.end()) {
                // Create a new state.
                GraphNode newState;
                newState.id = nodes.size();
                newState.blockId = targetBlockId;
                newState.blockSize = blocks[targetBlockId].size;
                newState.dp = newDP;
                newState.cc = newCC;
                nodes.push_back(newState);
                targetNodeId = newState.id;
                stateMap[key] = targetNodeId;
                worklist.push_back(targetNodeId);
            } else {
                targetNodeId = stateMap[key];
            }
            // Add an edge from the current state to the target state with the computed command.
            GraphEdge edge;
            edge.targetNode = targetNodeId;
            edge.command = cmd;
            nodes[curId].transitions.push_back(edge);
        }
    }
}

const std::vector<GraphNode>& Graph::getNodes() const {
    return nodes;
}
