#include "neuronGroup.h"

// Standard includes
#include <algorithm>
#include <cmath>

// GeNN includes
#include "codeGenUtils.h"
#include "standardSubstitutions.h"
#include "utils.h"

// ------------------------------------------------------------------------
// NeuronGroup
// ------------------------------------------------------------------------
void NeuronGroup::checkNumDelaySlots(unsigned int requiredDelay)
{
    if (requiredDelay >= getNumDelaySlots())
    {
        m_NumDelaySlots = requiredDelay + 1;
    }
}

void NeuronGroup::updateVarQueues(const string &code)
{
    // Loop through neuron variables
    for(const auto &v : getNeuronModel()->GetVars()) {
        // If the code contains a reference to this variable, set queued flag
        if (code.find(v.first + "_pre") != string::npos) {
            m_VarQueueRequired.insert(v.first);
        }
    }
}

void NeuronGroup::setVarZeroCopyEnabled(const std::string &var, bool enabled)
{
    // If named variable doesn't exist give error
    VarNameIterCtx nmVars(getNeuronModel()->GetVars());
    if(find(nmVars.nameBegin, nmVars.nameEnd, var) == nmVars.nameEnd) {
        gennError("Cannot find variable " + var);
    }
    // Otherwise add name of variable to set
    else  {
        // If enabled, add variable to set
        if(enabled) {
            m_VarZeroCopyEnabled.insert(var);
        }
        // Otherwise, remove it
        else {
            m_VarZeroCopyEnabled.erase(var);
        }
    }
}


void NeuronGroup::addSpkEventCondition(const std::string &code, const std::string &supportCodeNamespace)
{
    m_SpikeEventCondition.insert(std::pair<std::string, std::string>(code, supportCodeNamespace));
}

void NeuronGroup::initDerivedParams(double dt)
{
    auto derivedParams = getNeuronModel()->GetDerivedParams();

    // Reserve vector to hold derived parameters
    m_DerivedParams.reserve(derivedParams.size());

    // Loop through derived parameters
    for(const auto &d : derivedParams) {
        m_DerivedParams.push_back(d.second(m_Params, dt));
    }
}

void NeuronGroup::calcSizes(unsigned int blockSize, unsigned int &cumSum, unsigned int &paddedCumSum)
{
    // paddedSize is the lowest multiple of neuronBlkSz >= neuronN[i]
    const unsigned int paddedSize = ceil((double)getNumNeurons() / (double) blockSize) * (double) blockSize;

    // Store current cummulative sum in first
    m_CumSumNeurons.first = cumSum;
    m_PaddedCumSumNeurons.first = paddedCumSum;

    // Update global cummulative sums of neurons
    cumSum += getNumNeurons();
    paddedCumSum +=  paddedSize;

    // Store cummulative sums of point after this neuron group
    m_CumSumNeurons.second = cumSum;
    m_PaddedCumSumNeurons.second = paddedCumSum;
}

bool NeuronGroup::isVarQueueRequired(const std::string &var) const
{
    return (m_VarQueueRequired.find(var) != std::end(m_VarQueueRequired));
}

bool NeuronGroup::isZeroCopyEnabled() const
{
    // If any bits of spikes require zero-copy return true
    if(isSpikeZeroCopyEnabled() || isSpikeEventZeroCopyEnabled() || isSpikeTimeZeroCopyEnabled())
    {
        return true;
    }

    // If there are any variables return true
    if(!m_VarZeroCopyEnabled.empty())
    {
        return true;
    }

    return false;
}

bool NeuronGroup::isVarZeroCopyEnabled(const std::string &var) const
{
    return (m_VarZeroCopyEnabled.find(var) != std::end(m_VarZeroCopyEnabled));
}

void NeuronGroup::addExtraGlobalParams(std::map<string, string> &kernelParameters) const
{
    for(auto const &p : getNeuronModel()->GetExtraGlobalParams()) {
        std::string pnamefull = p.first + getName();
        if (kernelParameters.find(pnamefull) == kernelParameters.end()) {
            // parameter wasn't registered yet - is it used?
            if (getNeuronModel()->GetSimCode().find("$(" + p.first + ")") != string::npos
                || getNeuronModel()->GetThresholdConditionCode().find("$(" + p.first + ")") != string::npos
                || getNeuronModel()->GetResetCode().find("$(" + p.first + ")") != string::npos) {
                kernelParameters.insert(pair<string, string>(pnamefull, p.second));
            }
        }
    }
}

void NeuronGroup::addSpikeEventConditionParams(const std::pair<std::string, std::string> &param,
                                               std::map<string, string> &kernelParameters) const
{
    std::string pnamefull = param.first + getName();
    if (kernelParameters.find(pnamefull) == kernelParameters.end()) {
        // parameter wasn't registered yet - is it used?
        bool used = false;

        // Loop through event conditions
        for(const auto &spkEventCond : m_SpikeEventCondition) {
            // If the event threshold code contains this parameter
            // (in it's non-uniquified form), set flag and stop searching
            if(spkEventCond.first.find(pnamefull) != string::npos) {
                used = true;
                break;
            }
        }

        if (used) {
            kernelParameters.insert(pair<string, string>(pnamefull, param.second));
        }
    }
}

std::string NeuronGroup::getQueueOffset(const std::string &devPrefix) const
{
    return isDelayRequired()
        ? "(" + devPrefix + "spkQuePtr" + getName() + " * " + to_string(getNumNeurons()) + ") + "
        : "";
}