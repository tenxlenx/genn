#include "spineMLNeuronModel.h"

// Standard C++ includes
#include <algorithm>
#include <iostream>
#include <sstream>

// Standard C includes
#include <cstring>

// pugixml includes
#include "pugixml/pugixml.hpp"

// GeNN includes
#include "CodeHelper.h"

//----------------------------------------------------------------------------
// SpineMLGenerator::SpineMLNeuronModel
//----------------------------------------------------------------------------
SpineMLGenerator::SpineMLNeuronModel::SpineMLNeuronModel(const std::string &url,
                                                         const std::set<std::string> &variableParams)
{
    // Load XML document
    pugi::xml_document doc;
    auto result = doc.load_file(url.c_str());
    if(!result) {
        throw std::runtime_error("Could not open file:" + url + ", error:" + result.description());
    }

    // Get SpineML root
    auto spineML = doc.child("SpineML");
    if(!spineML) {
        throw std::runtime_error("XML file:" + url + " is not a SpineML component - it has no root SpineML node");
    }

    // Get component class
    auto componentClass = spineML.child("ComponentClass");
    if(!componentClass || strcmp(componentClass.attribute("type").value(), "neuron_body") != 0) {
        throw std::runtime_error("XML file:" + url + " is not a SpineML neuron body component - it's ComponentClass node is either missing or of the incorrect type");
    }

    std::cout << "\t\tModel name:" << componentClass.attribute("name").value() << std::endl;

    // Build mapping from regime names to IDs
    auto dynamics = componentClass.child("Dynamics");
    std::map<std::string, unsigned int> regimeIDs;
    std::transform(dynamics.children("Regime").begin(), dynamics.children("Regime").end(),
                   std::inserter(regimeIDs, regimeIDs.end()),
                   [&regimeIDs](const pugi::xml_node &n)
                   {
                       return std::make_pair(n.attribute("name").value(), regimeIDs.size());
                   });
    const bool multipleRegimes = (regimeIDs.size() > 1);

    // Starting with those the model needs to vary, create a set of genn variables
    std::set<string> gennVariables(variableParams);

    // Add model state variables to this
    std::transform(dynamics.children("StateVariable").begin(), dynamics.children("StateVariable").end(),
                   std::inserter(gennVariables, gennVariables.end()),
                   [](const pugi::xml_node &n){ return n.attribute("name").value(); });

    // Loop through model parameters
    std::cout << "\t\tParameters:" << std::endl;
    for(auto param : componentClass.children("Parameter")) {
        // If parameter hasn't been declared variable by model, add it to vector of parameter names
        const auto *paramName = param.attribute("name").value();
        if(gennVariables.find(paramName) == gennVariables.end()) {
            std::cout << "\t\t\t" << paramName << std::endl;
            m_ParamNames.push_back(paramName);
        }
    }

    // Add all GeNN variables
    std::transform(gennVariables.begin(), gennVariables.end(), std::back_inserter(m_Vars),
                   [](const std::string &vname){ return std::make_pair(vname, "scalar"); });

    // If model has multiple regimes, add unsigned int regime ID to values
    if(multipleRegimes) {
        m_Vars.push_back(std::make_pair("_regimeID", "unsigned int"));
    }

    std::cout << "\t\tVariables:" << std::endl;
    for(const auto &v : m_Vars) {
        std::cout << "\t\t\t" << v.first << ":" << v.second << std::endl;
    }

    // Loop through regimes
    std::stringstream simCode;
    std::stringstream thresholdCondition;
    CodeHelper hlp;
    bool firstRegime = true;
    std::cout << "\t\tRegimes:" << std::endl;
    for(auto regime : dynamics.children("Regime")) {
        const auto *regimeName = regime.attribute("name").value();
        std::cout << "\t\t\tRegime name:" << regimeName << ", id:" << regimeIDs[regimeName] << std::endl;

        // Write regime condition test code to sim code
        if(multipleRegimes) {
            if(firstRegime) {
                firstRegime = false;
            }
            else {
                simCode << "else ";
            }
            simCode << "if(_regimeID == " << regimeIDs[regimeName] << ")" << OB(1);
        }

        // Loop through conditions by which neuron might leave regime
        for(auto condition : regime.children("OnCondition")) {
            const auto *targetRegimeName = condition.attribute("target_regime").value();

            // Get triggering code
            auto triggerCode = condition.child("Trigger").child("MathInline");
            if(!triggerCode) {
                throw std::runtime_error("No trigger condition for transition between regimes");
            }

            // Write trigger condition
            simCode << "if(" << triggerCode.text().get() << ")" << OB(2);

            // Loop through state assignements
            for(auto stateAssign : condition.children("StateAssignment")) {
                simCode << stateAssign.attribute("variable").value() << " = " << stateAssign.child_value("MathInline") << ";" << ENDL;
            }

            // If this is a multiple-regime model, write transition to target regime
            if(multipleRegimes) {
                simCode << "_regimeID = " << regimeIDs[targetRegimeName] << ";" << ENDL;
            }
            // Otherwise check condition is targetting current regime
            else if(strcmp(targetRegimeName, regimeName) != 0) {
                throw std::runtime_error("Condition found in single-regime model which doesn't target itself");
            }

            // End of trigger condition
            simCode << CB(2);

            // If this condition emits a spike
            auto spikeEventOut = condition.select_node("EventOut[@port='spike']");
            if(spikeEventOut) {
                // If there are existing threshold conditions, OR them with this one
                if(thresholdCondition.tellp() > 0) {
                    thresholdCondition << " || ";
                }

                // Write trigger condition AND regime to threshold condition
                thresholdCondition << "(_regimeID == " << regimeIDs[regimeName] << " && (" << triggerCode.text().get() << "))";
            }
        }

        // Write dynamics
        // **TODO** identify cases where Euler is REALLY stupid
        auto timeDerivative = regime.child("TimeDerivative");
        if(timeDerivative) {
            simCode << timeDerivative.attribute("variable").value() << " += DT * (" << timeDerivative.child_value("MathInline") << ");" << ENDL;
        }

        // End of regime
        if(multipleRegimes) {
            simCode << CB(1);
        }
    }

    // Store generated code in class
    m_SimCode = simCode.str();
    m_ThresholdConditionCode = thresholdCondition.str();

    std::cout << "SIM CODE:" << std::endl << m_SimCode << std::endl;
    std::cout << "THRESHOLD CONDITION CODE:" << std::endl << m_ThresholdConditionCode << std::endl;
}



//----------------------------------------------------------------------------
// SpineMLGenerator::SpineMLNeuronModel::ParamValues
//----------------------------------------------------------------------------
std::vector<double> SpineMLGenerator::SpineMLNeuronModel::ParamValues::getValues() const
{
    // Reserve vector to hold values
    std::vector<double> values;
    values.reserve(m_Values.size());

    // Transform values of value map into vector and return
    std::transform(std::begin(m_Values), std::end(m_Values),
                   std::back_inserter(values),
                   [](const std::pair<std::string, double> &v){ return v.second; });
    return values;
}