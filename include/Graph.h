#ifndef GRAPH_H
#define GRAPH_H

#include <vector>
#include <utility>
#include "PietTypes.h"

// Each edge records the target node (a state) and the command executed to transition.
struct GraphEdge {
    int targetNode;      // index of the target GraphNode in the graph's node vector
    Command command;     // the command that was executed on the transition
};

// Each GraphNode represents a program state: a particular block plus the DP and CC at that time.
struct GraphNode {
    int id;              // Unique node id.
    int blockId;         // The id of the corresponding color block (from connected components).
    int blockSize;       // The number of codels in the block (for use in the Push command).
    Direction dp;        // The current direction pointer.
    CodelChooser cc;     // The current codel chooser.
    std::vector<GraphEdge> transitions;  // One or more outgoing transitions.
};

class Graph {
public:
    Graph();
    // Build the execution graph from the grid of PietColors.
    void buildGraph(const std::vector<std::vector<PietColor>> &grid);
    // Return the computed nodes.
    const std::vector<GraphNode>& getNodes() const;
private:
    // A “block” is a connected region (by 4–connectivity) of codels having the same color.
    struct Block {
        int id;
        PietColor color;
        int size;
        std::vector<std::pair<int,int>> cells; // (row, col) coordinates in the grid.
    };
    std::vector<GraphNode> nodes;
    std::vector<Block> blocks; // Computed connected color blocks.
    
    // Compute connected color blocks from the grid.
    void computeBlocks(const std::vector<std::vector<PietColor>> &grid);
    // Given two colors (from and to), compute the Piet command according to the specification.
    Command getCommand(PietColor from, PietColor to);

    // --- Simulation Helpers ---
    // Given a block, a DP, and a CC, choose the "exit codel" from the block.
    std::pair<int,int> getExitCodel(const Block &block, Direction dp, CodelChooser cc,
                                      const std::vector<std::vector<PietColor>> &grid);
    // Given a coordinate and a DP, return the adjacent coordinate in that direction.
    std::pair<int,int> getNextCodel(const std::pair<int,int>& coord, Direction dp);
    // Given a coordinate (r,c), return the id of the block that contains that coordinate, or -1 if none.
    int findBlockId(int r, int c) const;
    // Rotate a given DP by (clockwise) n steps.
    Direction rotateDP(Direction dp, int steps);
    // Toggle the codel chooser.
    CodelChooser toggleCC(CodelChooser cc);
};

#endif // GRAPH_H
