// Standard C++ includes
#include <iostream>
#include <map>
#include <set>
#include <string>

// Standard C includes
#include <cassert>
#include <cstdlib>

// Filesystem includes
#include "filesystem/path.h"

// pugixml includes
#include "pugixml/pugixml.hpp"

// GeNN includes
#include "generateALL.h"
#include "global.h"
#include "modelSpec.h"
#include "utils.h"

// SpineMLCommon includes
#include "connectors.h"
#include "spineMLUtils.h"

// SpineMLGenerator includes
#include "spineMLNeuronModel.h"
#include "spineMLPostsynapticModel.h"
#include "spineMLWeightUpdateModel.h"

using namespace SpineMLCommon;
using namespace SpineMLGenerator;

//----------------------------------------------------------------------------
// Anonymous namespace
//----------------------------------------------------------------------------
namespace
{
typedef std::pair<std::string, std::set<std::string>> ModelParams;

//----------------------------------------------------------------------------
// Functions
//----------------------------------------------------------------------------
// Helper function to take a SpineML model and determine which of it's parameters are fixed and thus can
// potentially be hardcoded in GeNN or variable thus need to be implemented as GeNN model variables
std::tuple<ModelParams, std::map<std::string, double>> readModelProperties(const filesystem::path &basePath,
                                                                           const pugi::xml_node &node)
{
    // Build uniquely identifying model parameters starting with its 'url'
    ModelParams modelParams;
    modelParams.first = (basePath / node.attribute("url").value()).str();

    // Determine which properties are variable (therefore
    // can't be substituted directly into auto-generated code)
    std::map<std::string, double> fixedParamVals;
    for(auto param : node.children("Property")) {
        const auto *paramName = param.attribute("name").value();

        // If parameter has a fixed value, it can be hard-coded into either model or automatically initialized in simulator
        // **TODO** annotation to say you don't want this to be hard-coded
        auto fixedValue = param.child("FixedValue");
        if(fixedValue) {
            fixedParamVals.insert(std::make_pair(paramName, fixedValue.attribute("value").as_double()));
        }
        // Otherwise, in GeNN terms, it should be treated as a variable
        else {
            modelParams.second.insert(paramName);
        }
    }

    return std::make_tuple(modelParams, fixedParamVals);
}
//----------------------------------------------------------------------------
// Helper function to either find existing model that provides desired parameters or create new one
template<typename T>
const T &getCreateModel(const ModelParams &params, std::map<ModelParams, T> &models)
{
    // If no existing model is found that matches parameters
    const auto existingModel = models.find(params);
    if(existingModel == models.end())
    {
        // Create new model
        std::cout << "\tCreating new model" << std::endl;
        auto newModel = models.insert(
            std::make_pair(params, T(params.first, params.second)));

        return newModel.first->second;
    }
    else
    {
        return existingModel->second;
    }
}
//----------------------------------------------------------------------------
unsigned int getNeuronPopSize(const std::string &popName, const std::map<std::string, unsigned int> &popSizes)
{
    auto pop = popSizes.find(popName);
    if(pop == popSizes.end()) {
        throw std::runtime_error("Cannot find neuron population:" + popName);
    }
    else {
        return pop->second;
    }
}
//----------------------------------------------------------------------------
// Helper function to read the delay value from a SpineML 'Synapse' node
unsigned int readDelaySteps(const pugi::xml_node &node, double dt)
{
    // Get delay node
    auto delay = node.child("Delay");
    if(delay) {
        auto fixedValue = delay.child("FixedValue");
        if(fixedValue) {
            double delay = fixedValue.attribute("value").as_double();
            return (unsigned int)std::round(delay / dt);
        }
        else {
            throw std::runtime_error("GeNN currently only supports projections with a single delay value");
        }
    }
    else
    {
        throw std::runtime_error("Connector has no 'Delay' node");
    }
}
//----------------------------------------------------------------------------
// Helper function to determine the correct type of GeNN projection to use for a SpineML 'Synapse' node
std::tuple<SynapseMatrixType, unsigned int, unsigned int> getSynapticMatrixType(const filesystem::path &basePath, const pugi::xml_node &node,
                                                                                unsigned int numPre, unsigned int numPost, bool globalG, double dt)
{
    auto oneToOne = node.child("OneToOneConnection");
    if(oneToOne) {
        return std::make_tuple(Connectors::OneToOne::getMatrixType(oneToOne, numPre, numPost, globalG),
                               readDelaySteps(oneToOne, dt),
                               Connectors::OneToOne::estimateMaxRowLength(oneToOne, numPre, numPost));
    }

    auto allToAll = node.child("AllToAllConnection");
    if(allToAll) {
        return std::make_tuple(Connectors::AllToAll::getMatrixType(allToAll, numPre, numPost, globalG),
                               readDelaySteps(allToAll, dt),
                               Connectors::AllToAll::estimateMaxRowLength(allToAll, numPre, numPost));
    }

    auto fixedProbability = node.child("FixedProbabilityConnection");
    if(fixedProbability) {
        return std::make_tuple(Connectors::FixedProbability::getMatrixType(fixedProbability, numPre, numPost, globalG),
                               readDelaySteps(fixedProbability, dt),
                               Connectors::FixedProbability::estimateMaxRowLength(fixedProbability, numPre, numPost));
    }

    auto connectionList = node.child("ConnectionList");
    if(connectionList) {
        return std::make_tuple(Connectors::List::getMatrixType(fixedProbability, numPre, numPost, globalG),
                               readDelaySteps(fixedProbability, dt),
                               Connectors::List::estimateMaxRowLength(basePath, fixedProbability, numPre, numPost));
    }

    throw std::runtime_error("No supported connection type found for projection");
}
}   // Anonymous namespace

//----------------------------------------------------------------------------
// Entry point
//----------------------------------------------------------------------------
int main(int argc,
         char *argv[])
{
    if(argc != 2) {
        std::cerr << "Expected model XML file passed as argument" << std::endl;
        return EXIT_FAILURE;
    }

#ifndef CPU_ONLY
    CHECK_CUDA_ERRORS(cudaGetDeviceCount(&deviceCount));
    deviceProp = new cudaDeviceProp[deviceCount];
    for (int device = 0; device < deviceCount; device++) {
        CHECK_CUDA_ERRORS(cudaSetDevice(device));
        CHECK_CUDA_ERRORS(cudaGetDeviceProperties(&(deviceProp[device]), device));
    }
#endif // CPU_ONLY

    // Use filesystem library to get parent path of the network XML file
    auto networkPath = filesystem::path(argv[1]);
    auto basePath = networkPath.parent_path();

    // Load XML document
    pugi::xml_document doc;
    auto result = doc.load_file(networkPath.str().c_str());
    if(!result) {
        throw std::runtime_error("Unable to load XML file:" + networkPath.str() + ", error:" + result.description());
    }

    // Get SpineML root
    auto spineML = doc.child("LL:SpineML");
    if(!spineML) {
        throw std::runtime_error("XML file:" + networkPath.str() + " is not a low-level SpineML network - it has no root SpineML node");
    }

    // Neuron, postsyaptic and weight update models required by network
    std::map<ModelParams, SpineMLNeuronModel> neuronModels;
    std::map<ModelParams, SpineMLPostsynapticModel> postsynapticModels;
    std::map<ModelParams, SpineMLWeightUpdateModel> weightUpdateModels;

    // Get the filename of the network and remove extension
    // to get something usable as a network name
    std::string networkName = networkPath.filename();
    networkName = networkName.substr(0, networkName.find_last_of("."));

    // Instruct GeNN to export all functions as extern "C"
    GENN_PREFERENCES::buildSharedLibrary = true;

    // Initialize GeNN
    initGeNN();

    // The neuron model
    NNmodel model;
    model.setDT(0.1);
    model.setName(networkName);

    // Loop through populations once to build neuron populations
    std::map<std::string, unsigned int> neuronPopulationSizes;
    for(auto population : spineML.children("LL:Population")) {
        auto neuron = population.child("LL:Neuron");
        if(!neuron) {
            throw std::runtime_error("'Population' node has no 'Neuron' node");
        }

        // Read basic population properties
        auto popName = SpineMLUtils::getSafeName(neuron.attribute("name").value());
        const unsigned int popSize = neuron.attribute("size").as_int();
        std::cout << "Population " << popName << " consisting of ";
        std::cout << popSize << " neurons" << std::endl;

        // Add size to dictionary
        neuronPopulationSizes.insert(std::make_pair(popName, popSize));

        // If population is a spike source add GeNN spike source
        // **TODO** is this the only special case?
        if(strcmp(neuron.attribute("url").value(), "SpikeSource") == 0) {
            model.addNeuronPopulation<NeuronModels::SpikeSource>(popName, popSize, {}, {});
        }
        else {
            // Read neuron properties
            ModelParams modelParams;
            std::map<std::string, double> fixedParamVals;
            tie(modelParams, fixedParamVals) = readModelProperties(basePath, neuron);

            // Either get existing neuron model or create new one of no suitable models are available
            const auto &neuronModel = getCreateModel(modelParams, neuronModels);

            // Add population to model
            model.addNeuronPopulation(popName, popSize, &neuronModel,
                                      SpineMLNeuronModel::ParamValues(fixedParamVals, neuronModel),
                                      SpineMLNeuronModel::VarValues(fixedParamVals, neuronModel));
        }
    }

    // Loop through populations AGAIN to build projections
    for(auto population : spineML.children("LL:Population")) {
        // Read source population name from neuron node
        auto srcPopName = SpineMLUtils::getSafeName(population.child("LL:Neuron").attribute("name").value());
        const unsigned int srcPopSize = getNeuronPopSize(srcPopName, neuronPopulationSizes);

        // Loop through outgoing projections
        for(auto projection : population.children("LL:Projection")) {
            // Read destination population name from projection
            auto trgPopName = SpineMLUtils::getSafeName(projection.attribute("dst_population").value());
            const unsigned int trgPopSize = getNeuronPopSize(trgPopName, neuronPopulationSizes);

            std::cout << "Projection from population:" << srcPopName << "->" << trgPopName << std::endl;

            // Get main synapse node
            auto synapse = projection.child("LL:Synapse");
            if(!synapse) {
                throw std::runtime_error("'Projection' node has no 'Synapse' node");
            }

            // Get weight update
            auto weightUpdate = synapse.child("LL:WeightUpdate");
            if(!weightUpdate) {
                throw std::runtime_error("'Synapse' node has no 'WeightUpdate' node");
            }

            // Read weight update properties
            ModelParams weightUpdateModelParams;
            std::map<std::string, double> fixedWeightUpdateParamVals;
            tie(weightUpdateModelParams, fixedWeightUpdateParamVals) = readModelProperties(basePath, weightUpdate);

            // Global weight value can be used if there are no variable parameters
            const bool globalG = weightUpdateModelParams.second.empty();

            // Either get existing postsynaptic model or create new one of no suitable models are available
            const auto &weightUpdateModel = getCreateModel(weightUpdateModelParams, weightUpdateModels);

            // Get post synapse
            auto postSynapse = synapse.child("LL:PostSynapse");
            if(!postSynapse) {
                throw std::runtime_error("'Synapse' node has no 'PostSynapse' node");
            }

            // Read postsynapse properties
            ModelParams postsynapticModelParams;
            std::map<std::string, double> fixedPostsynapticParamVals;
            tie(postsynapticModelParams, fixedPostsynapticParamVals) = readModelProperties(basePath, postSynapse);

            // Either get existing postsynaptic model or create new one of no suitable models are available
            const auto &postsynapticModel = getCreateModel(postsynapticModelParams, postsynapticModels);

            // Determine the GeNN matrix type and number of delay steps
            SynapseMatrixType mtype;
            unsigned int delaySteps;
            unsigned int maxConnections;
            tie(mtype, delaySteps, maxConnections) = getSynapticMatrixType(basePath, synapse, srcPopSize, trgPopSize, globalG, 0.1);

            // Add synapse population to model
            std::string synapsePopName = std::string(srcPopName) + "_" + trgPopName;
            auto synapsePop = model.addSynapsePopulation(synapsePopName, mtype, delaySteps, srcPopName, trgPopName,
                                                         &weightUpdateModel, SpineMLWeightUpdateModel::ParamValues(fixedWeightUpdateParamVals, weightUpdateModel), SpineMLWeightUpdateModel::VarValues(fixedWeightUpdateParamVals, weightUpdateModel),
                                                         &postsynapticModel, SpineMLPostsynapticModel::ParamValues(fixedPostsynapticParamVals, postsynapticModel), SpineMLPostsynapticModel::VarValues(fixedPostsynapticParamVals, postsynapticModel));

            // If matrix uses sparse connectivity set max connections
            if(mtype & SynapseMatrixConnectivity::SPARSE) {
                synapsePop->setMaxConnections(maxConnections);
            }
        }
    }

    // Finalize model
    model.finalize();

#ifndef CPU_ONLY
    chooseDevice(model, basePath.str());
#endif // CPU_ONLY
    generate_model_runner(model, basePath.str());

    // Build path to generated model code
    auto modelPath = basePath / (networkName + "_CODE");

    // Use this to build command line for building generated code
    std::string cmd = "cd " + modelPath.str() + " && ";
#ifdef _WIN32
    cmd += "nmake /nologo clean all";
#else // UNIX
    cmd += "make clean all";
#endif

#ifdef CPU_ONLY
    cmd += " CPU_ONLY=1";
#endif  // CPU_ONLY

    // Execute command
    int retval = system(cmd.c_str());
    if (retval != 0){
        throw std::runtime_error("Building generated code with call:'" + cmd + "' failed with return value:" + std::to_string(retval));
    }

    return EXIT_SUCCESS;
}