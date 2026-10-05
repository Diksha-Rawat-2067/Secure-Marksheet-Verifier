enum class Direction { LEFT, RIGHT };
struct ProofStep {
    std::string sibling_hash;
    Direction direction;
    int level;
    ProofStep(std::string h, Direction d, int lvl)
        : sibling_hash(h), direction(d), level(lvl) {}
};
struct MerkleNode {
    std::string hash;
    MerkleNode* left;
    MerkleNode* right;
    MerkleNode* parent;
    bool is_leaf;
    std::string student_id;
    std::string roll_no;
    MerkleNode(const std::string& h, bool leaf = false,
               const std::string& id = "", const std::string& roll = "")
        : hash(h), left(nullptr), right(nullptr), parent(nullptr),
          is_leaf(leaf), student_id(id), roll_no(roll) {}
    ~MerkleNode() {
        delete left;
        delete right;
    }
};
class BinaryHashTree {
private:
    MerkleNode* root_;
    std::vector<MerkleNode*> leaves_;
    std::vector<std::vector<MerkleNode*>> layers_;
    std::unordered_map<std::string, size_t> id_to_index_;
    std::unordered_map<std::string, StudentRecord> records_;
    void clear() {
        delete root_;
        root_ = nullptr;
        leaves_.clear();
        layers_.clear();
        id_to_index_.clear();
        records_.clear();
    }
public:
    BinaryHashTree() : root_(nullptr) {}
    ~BinaryHashTree() { clear(); }
    // Bottom-Up Tree Construction
    void buildTree(const std::vector<StudentRecord>& students) {
        clear();
        if (students.empty()) return;
        for (const auto& s : students) {
            records_[s.student_id] = s;
        }
        std::vector<MerkleNode*> current_layer;
        current_layer.reserve(students.size());
        // 1. Create Leaf Nodes
        for (size_t i = 0; i < students.size(); ++i) {
            std::string leaf_hash = students[i].computeLeafHash();
            MerkleNode* leaf = new MerkleNode(leaf_hash, true, students[i].student_id, students[i].roll_no);
            current_layer.push_back(leaf);
            leaves_.push_back(leaf);
            id_to_index_[students[i].student_id] = i;
            id_to_index_[students[i].roll_no] = i;
        }
        layers_.push_back(current_layer);
        // 2. Pair adjacent child hashes bottom-up
        while (current_layer.size() > 1) {
            std::vector<MerkleNode*> next_layer;
            // Handle odd node counts by cloning last node
            if (current_layer.size() % 2 != 0) {
                MerkleNode* last = current_layer.back();
                MerkleNode* clone = new MerkleNode(last->hash, last->is_leaf, last->student_id, last->roll_no);
                current_layer.push_back(clone);
                layers_.back().push_back(clone);
            }
            for (size_t i = 0; i < current_layer.size(); i += 2) {
                MerkleNode* left_child = current_layer[i];
                MerkleNode* right_child = current_layer[i + 1];
                // Parent = CustomHash(Left + Right)
                std::string parent_hash = CustomHash::hash(left_child->hash + right_child->hash);
                MerkleNode* parent = new MerkleNode(parent_hash, false);
                parent->left = left_child;
                parent->right = right_child;
                left_child->parent = parent;
                right_child->parent = parent;
                next_layer.push_back(parent);
            }
            layers_.push_back(next_layer);
            current_layer = std::move(next_layer);
        }
        root_ = current_layer.empty() ? nullptr : current_layer[0];
    }
    std::string getRootHash() const {
        return root_ ? root_->hash : "EMPTY_TREE";
    }
    size_t getLeafCount() const {
        return leaves_.size();
    }
    // Generate O(log N) Merkle Inclusion Proof
    bool generateProof(const std::string& query, std::vector<ProofStep>& out_proof) const {
        out_proof.clear();
        auto it = id_to_index_.find(query);
        if (it == id_to_index_.end()) return false;
        size_t idx = it->second;
        MerkleNode* curr = leaves_[idx];
        int level = 1;
        while (curr && curr->parent) {
            MerkleNode* parent = curr->parent;
            if (parent->left == curr) {
                // Sibling is on the right
                out_proof.emplace_back(parent->right->hash, Direction::RIGHT, level);
            } else {
                // Sibling is on the left
                out_proof.emplace_back(parent->left->hash, Direction::LEFT, level);
            }
            curr = parent;
            level++;
        }
        return true;
    }
    // Mathematical Proof Verification (Independent of full database)
    static bool verifyProof(const std::string& leaf_hash,
                           const std::vector<ProofStep>& proof,
                           const std::string& expected_root) {
        std::string current_hash = leaf_hash;
        for (const auto& step : proof) {
            if (step.direction == Direction::LEFT) {
                current_hash = CustomHash::hash(step.sibling_hash + current_hash);
            } else {
                current_hash = CustomHash::hash(current_hash + step.sibling_hash);
            }
        }
        return (current_hash == expected_root);
    }
    // Display Tree Statistics
    void printTreeSummary() const {
        std::cout << "                 BINARY HASH TREE (MERKLE TREE) SUMMARY               \n";
        std::cout << " Total Student Records (Leaves): " << getLeafCount() << "\n";
        int height = leaves_.empty() ? 0 : static_cast<int>(std::ceil(std::log2(leaves_.size())) + 1);
        std::cout << " Tree Height                   : " << height << " Levels\n";
        std::cout << " Proof Complexity per Student  : O(log N) ~ " << (height > 0 ? height - 1 : 0) << " hashes\n";
        std::cout << " Published Merkle Root Hash    : \n   " << getRootHash() << "\n";
    }
    // Print Step-by-Step Proof Audit for a Student
    void printProofAudit(const std::string& query) const {
        auto it = id_to_index_.find(query);
        if (it == id_to_index_.end()) {
            std::cout << "[Error] Student record '" << query << "' not found.\n";
            return;
        }
        const StudentRecord& s = records_.at(leaves_[it->second]->student_id);
        std::string leaf_hash = s.computeLeafHash();
        std::vector<ProofStep> proof;
        generateProof(query, proof);
        std::cout << "         CRYPTOGRAPHIC PROOF AUDIT: " << s.name << " (" << s.roll_no << ")\n";
        std::cout << " Leaf Hash (H_0)  : " << leaf_hash << "\n";
        std::cout << " Proof Path Steps : " << proof.size() << " sibling hashes (O(log N))\n";
        std::string curr = leaf_hash;
        for (const auto& step : proof) {
            std::string dir_str = (step.direction == Direction::LEFT) ? "LEFT " : "RIGHT";
            std::cout << " Level " << step.level << " | Sibling (" << dir_str << "): " << step.sibling_hash << "\n";
            if (step.direction == Direction::LEFT) {
                curr = CustomHash::hash(step.sibling_hash + curr);
            } else {
                curr = CustomHash::hash(curr + step.sibling_hash);
            }
            std::cout << "         -> Computed Hash: " << curr << "\n";
        }
        std::cout << " Final Computed Root: " << curr << "\n";
        std::cout << " Expected Root Hash : " << getRootHash() << "\n";
        bool passed = (curr == getRootHash());
        std::cout << " Proof Result       : " << (passed ? "[ VALID (100% AUTHENTIC) ]" : "[ FAILED (TAMPERED) ]") << "\n";
    }
    // Demonstrate Tamper Detection
    void testTamperDetection(const std::string& student_id) const {
        auto it = id_to_index_.find(student_id);
        if (it == id_to_index_.end()) return;
        StudentRecord tampered = records_.at(student_id);
        std::cout << "                   SIMULATING MARKSHEET TAMPERING                     \n";
        std::cout << " Target Candidate : " << tampered.name << " (" << tampered.roll_no << ")\n";
        std::cout << " Original Programming Score : " << tampered.programming << "/100\n";
        // Tamper: alter programming mark from 80 to 99!
        tampered.programming = 99;
        std::cout << " Forged Paper Score        : " << tampered.programming << "/100  [MODIFIED]\n";
        std::string original_leaf = records_.at(student_id).computeLeafHash();
        std::string tampered_leaf = tampered.computeLeafHash();
        std::cout << " Original Leaf Hash: " << original_leaf << "\n";
        std::cout << " Tampered Leaf Hash: " << tampered_leaf << "  (Avalanche effect!)\n";
        std::vector<ProofStep> proof;
        generateProof(student_id, proof);
        bool isValid = verifyProof(tampered_leaf, proof, getRootHash());
        std::cout << " Verification Verdict: " << (isValid ? "AUTHENTIC" : "[! TAMPER DETECTED / PROOF FAILED !]") << "\n";
        std::cout << " Result: Single altered mark is mathematically caught by the Merkle Root!\n";
    }
};
