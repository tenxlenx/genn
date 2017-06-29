#include "spineMLModelCommon.h"

// Standard C++ includes
#include <algorithm>
#include <iostream>
#include <regex>

// SpineML generator includes
#include "objectHandler.h"

//----------------------------------------------------------------------------
// SpineMLGenerator::ParamValues
//----------------------------------------------------------------------------
std::vector<double> SpineMLGenerator::ParamValues::getValues() const
{
    // Get parameter names from model
    auto modelParamNames = m_Model.getParamNames();

    // Reserve vector of values to match it
    std::vector<double> paramValues;
    paramValues.reserve(modelParamNames.size());

    // Populate this vector with either values from map or 0s
    std::transform(modelParamNames.begin(), modelParamNames.end(),
                   std::back_inserter(paramValues),
                   [this](const std::string &n)
                   {
                       auto value = m_Values.find(n);
                       if(value == m_Values.end()) {
                           return 0.0;
                       }
                       else {
                           return value->second;
                       }
                   });
    return paramValues;
}

//----------------------------------------------------------------------------
// SpineMLGenerator::VarValues
//----------------------------------------------------------------------------
std::vector<double> SpineMLGenerator::VarValues::getValues() const
{
    // Get variables from model
    auto modelVars = m_Model.getVars();

    // Reserve vector of values to match it
    std::vector<double> varValues;
    varValues.reserve(modelVars.size());

    // Populate this vector with either values from map or 0s
    std::transform(modelVars.begin(), modelVars.end(),
                   std::back_inserter(varValues),
                   [this](const std::pair<std::string, std::string> &n)
                   {
                       auto value = m_Values.find(n.first);
                       if(value == m_Values.end()) {
                           return 0.0;
                       }
                       else {
                           return value->second;
                       }
                   });
    return varValues;
}

//----------------------------------------------------------------------------
// SpineMLGenerator::CodeStream
//----------------------------------------------------------------------------
void SpineMLGenerator::CodeStream::onRegimeEnd(bool multipleRegimes, unsigned int currentRegimeID)
{
    // If any code was written for this regime
    if(m_CurrentRegimeStream.tellp() > 0)
    {
        if(multipleRegimes) {
            if(m_FirstNonEmptyRegime) {
                m_FirstNonEmptyRegime = false;
            }
            else {
                m_CodeStream << "else ";
            }
            m_CodeStream << "if(_regimeID == " << currentRegimeID << ")" << CodeStream::OB(1);
        }

        // Write contents of current region code stream to main code stream
        m_CodeStream << m_CurrentRegimeStream.str();

        // Clear current regime code stream
        std::ostringstream().swap(m_CurrentRegimeStream);

        // End of regime
        if(multipleRegimes) {
            m_CodeStream << CodeStream::CB(1);
        }
    }
}

//----------------------------------------------------------------------------
// Helper functions
//----------------------------------------------------------------------------
bool SpineMLGenerator::generateModelCode(const pugi::xml_node &componentClass, ObjectHandler::Base &objectHandlerEvent,
                                         ObjectHandler::Base &objectHandlerCondition, ObjectHandler::Base &objectHandlerImpulse,
                                         ObjectHandler::Base &objectHandlerTimeDerivative,
                                         std::function<void(bool, unsigned int)> regimeEndFunc)
{
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

    // Loop through regimes
    std::cout << "\t\tRegimes:" << std::endl;
    for(auto regime : dynamics.children("Regime")) {
        const auto *currentRegimeName = regime.attribute("name").value();
        const unsigned int currentRegimeID = regimeIDs[currentRegimeName];
        std::cout << "\t\t\tRegime name:" << currentRegimeName << ", id:" << currentRegimeID << std::endl;

        // Loop through internal conditions by which model might leave regime
        for(auto condition : regime.children("OnCondition")) {
            const auto *targetRegimeName = condition.attribute("target_regime").value();
            const unsigned int targetRegimeID = regimeIDs[targetRegimeName];
            objectHandlerCondition.onObject(condition, currentRegimeID, targetRegimeID);
        }

        // Loop through events the model might receive
        for(auto event : regime.children("OnEvent")) {
            const auto *targetRegimeName = event.attribute("target_regime").value();
            const unsigned int targetRegimeID = regimeIDs[targetRegimeName];
            objectHandlerEvent.onObject(event, currentRegimeID, targetRegimeID);
        }

        // Loop through impulses the model might receive
        for(auto impulse : regime.children("OnImpulse")) {
            const auto *targetRegimeName = impulse.attribute("target_regime").value();
            const unsigned int targetRegimeID = regimeIDs[targetRegimeName];
            objectHandlerImpulse.onObject(impulse, currentRegimeID, targetRegimeID);
        }

        // Write out time derivatives
        for(auto timeDerivative : regime.children("TimeDerivative")) {
            objectHandlerTimeDerivative.onObject(timeDerivative, currentRegimeID, 0);
        }

        // Call function to notify all code streams of end of regime
        regimeEndFunc(multipleRegimes, currentRegimeID);
    }

    return multipleRegimes;
}
//----------------------------------------------------------------------------
void SpineMLGenerator::wrapAndReplaceVariableNames(std::string &code, const std::string &variableName,
                                                   const std::string &replaceVariableName)
{
    // Build a regex to match variable name with at least one
    // character that can't be in a variable name on either side (or an end/beginning of string)
    std::regex regex("(^|[^a-zA-Z_])" + variableName + "($|[^a-zA-Z_])");

    // Insert GeNN $(XXXX) wrapper around variable name
    code = std::regex_replace(code,  regex, "$1$(" + replaceVariableName + ")$2");
}
//----------------------------------------------------------------------------
void SpineMLGenerator::wrapVariableNames(std::string &code, const std::string &variableName)
{
    wrapAndReplaceVariableNames(code, variableName, variableName);
}
//----------------------------------------------------------------------------
std::tuple<NewModels::Base::StringVec, NewModels::Base::StringPairVec> SpineMLGenerator::findModelVariables(
    const pugi::xml_node &componentClass, const std::set<std::string> &variableParams, bool multipleRegimes)
{
    // Starting with those the model needs to vary, create a set of genn variables
    std::set<std::string> gennVariables(variableParams);

    // Add model state variables to this
    auto dynamics = componentClass.child("Dynamics");
    std::transform(dynamics.children("StateVariable").begin(), dynamics.children("StateVariable").end(),
                   std::inserter(gennVariables, gennVariables.end()),
                   [](const pugi::xml_node &n){ return n.attribute("name").value(); });

    // Loop through model parameters
    NewModels::Base::StringVec paramNames;
    for(auto param : componentClass.children("Parameter")) {
        // If parameter hasn't been declared variable by model, add it to vector of parameter names
        std::string paramName = param.attribute("name").value();
        if(gennVariables.find(paramName) == gennVariables.end()) {
            paramNames.push_back(paramName);
        }
    }

    // Add all GeNN variables
    NewModels::Base::StringPairVec vars;
    std::transform(gennVariables.begin(), gennVariables.end(), std::back_inserter(vars),
                   [](const std::string &vname){ return std::make_pair(vname, "scalar"); });

    // If model has multiple regimes, add unsigned int regime ID to values
    if(multipleRegimes) {
        vars.push_back(std::make_pair("_regimeID", "unsigned int"));
    }

    // Return parameter names and variables
    return std::make_tuple(paramNames, vars);
}
//----------------------------------------------------------------------------
NewModels::Base::StringVec SpineMLGenerator::findAnalogueReceivePortNames(const pugi::xml_node &componentClass,
                                                                          const std::string &suffix)
{
     // Add analogue receive ports to this
    NewModels::Base::StringVec ports;
    std::transform(componentClass.children("AnalogReceivePort").begin(), componentClass.children("AnalogReceivePort").end(),
                   std::back_inserter(ports),
                   [suffix](const pugi::xml_node &n){ return n.attribute("name").value() + suffix; });
    return ports;

}
//----------------------------------------------------------------------------
void SpineMLGenerator::substituteModelVariables(const NewModels::Base::StringVec &paramNames,
                                                const NewModels::Base::StringPairVec &vars,
                                                const std::vector<std::string*> &codeStrings)
{
    // Loop through model parameters
    std::cout << "\t\tParameters:" << std::endl;
    for(const auto &p : paramNames) {
        std::cout << "\t\t\t" << p << std::endl;

        // Wrap variable names so GeNN code generator can find them
        for(std::string *c : codeStrings) {
            wrapVariableNames(*c, p);
        }
    }

    std::cout << "\t\tVariables:" << std::endl;
    for(const auto &v : vars) {
        std::cout << "\t\t\t" << v.first << ":" << v.second << std::endl;

        // Wrap variable names so GeNN code generator can find them
        for(std::string *c : codeStrings) {
            wrapVariableNames(*c, v.first);
        }
    }
}
//----------------------------------------------------------------------------
void SpineMLGenerator::substituteModelVariables(const NewModels::Base::StringVec &paramNames,
                                                const NewModels::Base::StringPairVec &vars,
                                                const NewModels::Base::StringVec &analogueReceivePortNames,
                                                const std::vector<std::string*> &codeStrings)
{
    // Substitute parameters and variables
    substituteModelVariables(paramNames, vars, codeStrings);

    // Loop through model parameters
    std::cout << "\t\tAnalogue receive ports:" << std::endl;
    for(const auto &p : analogueReceivePortNames) {
        std::cout << "\t\t\t" << p << std::endl;

        // Wrap variable names so GeNN code generator can find them
        for(std::string *c : codeStrings) {
            wrapVariableNames(*c, p);
        }
    }
}
//----------------------------------------------------------------------------
std::tuple<NewModels::Base::StringVec, NewModels::Base::StringPairVec> SpineMLGenerator::processModelVariables(
    const pugi::xml_node &componentClass, const std::set<std::string> &variableParams,
    bool multipleRegimes, const std::vector<std::string*> &codeStrings)
{
    // Find variables
    auto paramNamesVars = findModelVariables(componentClass, variableParams, multipleRegimes);

    // Use them to perform substitutions
    substituteModelVariables(std::get<0>(paramNamesVars), std::get<1>(paramNamesVars), codeStrings);

    return paramNamesVars;
}