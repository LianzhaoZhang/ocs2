/******************************************************************************
Copyright (c) 2017, Farbod Farshidian. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#include "ocs2_switched_model_interface/logic/ModeSequenceTemplate.h"

#include <ocs2_core/misc/Display.h>
#include <ocs2_core/misc/LoadData.h>

namespace switched_model {

std::ostream& operator<<(std::ostream& stream, const ModeSequenceTemplate& modeSequenceTemplate) {
  stream << "Template switching times: {" << ocs2::toDelimitedString(modeSequenceTemplate.switchingTimes) << "}\n";
  stream << "Template mode sequence:   {" << ocs2::toDelimitedString(modeSequenceTemplate.modeSequence) << "}\n";
  return stream;
}

ModeSequenceTemplate loadModeSequenceTemplate(const std::string& filename, const std::string& topicName, bool verbose) {
  std::vector<ModeSequenceTemplate::scalar_t> switchingTimes;
  std::vector<std::string> modeSequenceString;

  try {
    // switching times
    ocs2::loadData::loadStdVector(filename, topicName + ".switchingTimes", switchingTimes, verbose);

    // read the modes name
    ocs2::loadData::loadStdVector(filename, topicName + ".modeSequence", modeSequenceString, verbose);
  } catch (const std::exception& e) {
    std::cerr << "WARNING: Failed to load " + topicName + "!" << std::endl;
  }

  // convert the mode name to mode enum
  std::vector<size_t> modeSequence;
  modeSequence.reserve(modeSequenceString.size());
  for (const auto& modeName : modeSequenceString) {
    modeSequence.push_back(string2ModeNumber(modeName));
  }

  return {switchingTimes, modeSequence};
}

ocs2_msgs::mode_schedule createModeSequenceTemplateMsg(const ModeSequenceTemplate& modeSequenceTemplate) {
  ocs2_msgs::mode_schedule modeScheduleMsg;
  modeScheduleMsg.eventTimes.assign(modeSequenceTemplate.switchingTimes.begin(), modeSequenceTemplate.switchingTimes.end());
  modeScheduleMsg.modeSequence.assign(modeSequenceTemplate.modeSequence.begin(), modeSequenceTemplate.modeSequence.end());
  return modeScheduleMsg;
}

ModeSequenceTemplate readModeSequenceTemplateMsg(const ocs2_msgs::mode_schedule& modeScheduleMsg) {
  std::vector<ModeSequenceTemplate::scalar_t> switchingTimes(modeScheduleMsg.eventTimes.begin(), modeScheduleMsg.eventTimes.end());
  std::vector<size_t> modeSequence(modeScheduleMsg.modeSequence.begin(), modeScheduleMsg.modeSequence.end());
  return {switchingTimes, modeSequence};
}

}  // namespace switched_model
