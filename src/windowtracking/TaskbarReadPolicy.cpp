#include "windowtracking/TaskbarReadPolicy.h"

namespace polish {

ReadDecision DecideOnRead(ReadPolicyState& state, bool usable, std::size_t buttonCount, bool haveLastGood) {
    if (usable && buttonCount > 0) {
        state.consecutiveBadReads = 0;
        return ReadDecision::Apply;
    }
    if (!haveLastGood) {
        // Nothing was ever shielded, so there is nothing to keep or to
        // uncover. Apply whatever this is -- typically an empty picture --
        // and start counting from clean so a later good read is not
        // penalised by history that never mattered.
        state.consecutiveBadReads = 0;
        return ReadDecision::Apply;
    }
    ++state.consecutiveBadReads;
    return state.consecutiveBadReads >= kBadReadsBeforeDegrade ? ReadDecision::Degrade : ReadDecision::KeepLastGood;
}

}  // namespace polish
