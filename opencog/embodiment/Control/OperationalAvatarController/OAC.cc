/*
 * opencog/embodiment/Control/OperationalAvatarController/OAC.cc
 *
 * Copyright (C) 2002-2009 Novamente LLC
 * All Rights Reserved
 * Author(s): Carlos Lopes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */


#include <opencog/comboreduct/combo/type_tree.h>

#include <opencog/embodiment/Learning/LearningServerMessages/SchemaMessage.h>
#include <opencog/embodiment/PetComboVocabulary/PetComboVocabulary.h>

#include <opencog/util/files.h>
#include <opencog/util/Config.h>

#include <fstream>
#include <iostream>

#include "OAC.h"

/**
 * Uncoment the following define in order to delete atomSpace content inside OAC
 * destructor
 */
//#define DELETE_ATOMSPACE

using namespace OperationalAvatarController;
using namespace Procedure;
using namespace PetCombo;
using namespace opencog;

BaseServer* OAC::createInstance()
{
    return new OAC;
}

OAC::OAC() {}

void OAC::init(const std::string & myId, const std::string & ip, int portNumber,
               const std::string& petId, const std::string& ownerId,
               const std::string& agentType, const std::string& agentTraits)
{

    setNetworkElement(new NetworkElement(myId, ip, portNumber));

    std::string aType = (agentType == "pet" || agentType == "humanoid") ?
                        agentType : config().get( "RULE_ENGINE_DEFAULT_AGENT_TYPE" );


    this->planSender = new PVPActionPlanSender(petId, &(getNetworkElement()));
    this->petMessageSender = new PetMessageSender(&(getNetworkElement()));

    // load pet
    if (fileExists(getPath(petId, config().get("PET_DUMP")).c_str())) {
        loadPet(petId);
    } else {
        this->pet = new Pet(petId, config().get("UNKNOWN_PET_NAME"),
                            aType, agentTraits, ownerId,
                            atomSpace, petMessageSender);
    }

    this->pai = new PerceptionActionInterface::PAI(*atomSpace, *planSender, *pet);

    this->procedureRepository = new ProcedureRepository(*pai);
    this->procedureInterpreter = new ProcedureInterpreter(*pai);

    this->pet->setPAI(pai);

    // adds all savable repositories for further calls to save/load methods.
    savingLoading.addSavableRepository(procedureRepository);

    if (fileExists(getPath(petId, config().get("ATOM_SPACE_DUMP")).c_str())) {
        loadAtomSpace(petId);
    } else {
        pet->initTraitsAndFeelings();

        logger().info("OAC - Loading initial Combo stdlib file '%s', RulesPreconditions '%s' and ActionSchemataPreconditions '%s'.",
                     config().get("COMBO_STDLIB_REPOSITORY_FILE").c_str(),
                     config().get("COMBO_RULES_PRECONDITIONS_REPOSITORY_FILE").c_str(),
                     config().get("COMBO_RULES_ACTION_SCHEMATA_REPOSITORY_FILE").c_str());

        int cnt = 0;
        ifstream fin(config().get("COMBO_STDLIB_REPOSITORY_FILE").c_str());
        if (fin.good()) {
            cnt = procedureRepository->loadComboFromStream(fin);
        } else {
            logger().error(
                         "OAC - Unable to load Combo stdlib.");
        }
        fin.close();
        logger().info(
                     "OAC - %d Combo functions loaded.", cnt);

        fin.open(config().get("COMBO_RULES_PRECONDITIONS_REPOSITORY_FILE").c_str());
        if (fin.good()) {
            cnt = procedureRepository->loadComboFromStream(fin);
        } else {
            logger().error(
                         "OAC - Unable to load RulePreconditions combo.");
        }
        fin.close();
        logger().info(
                     "OAC - RulesPreconditions combo functions loaded.");

        fin.open(config().get("COMBO_SELECT_RULES_PRECONDITIONS_REPOSITORY_FILE").c_str());
        if (fin.good()) {
            cnt = procedureRepository->loadComboSelectFromStream(fin);
        } else {
            logger().error(
                         "OAC - Unable to load RulePreconditions combo select.");
        }
        fin.close();
        logger().info(
                     "OAC - RulesPreconditions combo select functions loaded.");

        fin.open(config().get("COMBO_RULES_ACTION_SCHEMATA_REPOSITORY_FILE").c_str());
        if (fin.good()) {
            cnt = procedureRepository->loadComboFromStream(fin);
        } else {
            logger().error(
                         "OAC - Unable to load RulesActionSchemata combo.");
        }
        fin.close();
        logger().info(
                     "OAC - RulesActionSchemata combo functions loaded.");
    }


    // warning: it must be called after register the agent and it's owner nodes
    this->ruleEngine = new RuleEngine( this, petId );
    this->pet->setRuleEngine(ruleEngine);

    predicatesUpdater = new PredicatesUpdater(*atomSpace, pet->getPetId());

    // TODO: remove component reference from component constructors

    // Register and create agents
    // IMPORTANT: the order the agents are created matters
    //            if they must be executed sequencially.

    this->registerAgent(ProcedureInterpreterAgent::info().id, &procedureInterpreterAgentFactory);
    procedureInterpreterAgent = static_cast<ProcedureInterpreterAgent*>(
                                    this->createAgent(ProcedureInterpreterAgent::info().id, false));
    procedureInterpreterAgent->setInterpreter(procedureInterpreter);

    this->registerAgent(ActionSelectionAgent::info().id, &actionSelectionAgentFactory);
    actionSelectionAgent = static_cast<ActionSelectionAgent*>(
                               this->createAgent(ActionSelectionAgent::info().id, false));

    this->registerAgent(ImportanceDecayAgent::info().id, &importanceDecayAgentFactory);
    importanceDecayAgent = static_cast<ImportanceDecayAgent*>(
                               this->createAgent(ImportanceDecayAgent::info().id, false));
    importanceDecayAgent->connectSignals(*atomSpace);

    this->registerAgent(EntityExperienceAgent::info().id, &entityExperienceAgentFactory);
    entityExperienceAgent = static_cast<EntityExperienceAgent*>(
                               this->createAgent(EntityExperienceAgent::info().id, false));

    if (config().get_bool("PROCEDURE_INTERPRETER_ENABLED")) {
        this->startAgent(procedureInterpreterAgent);
    }

    if (config().get_bool("ACTION_SELECTION_ENABLED")) {
        actionSelectionAgent->setFrequency(config().get_int("RE_CYCLE_PERIOD"));
        this->startAgent(actionSelectionAgent);
    }

    if (config().get_bool("PROCEDURE_INTERPRETER_ENABLED")) {
        // adds the same procedure interpreter agent to schedule again
        this->startAgent(procedureInterpreterAgent);
    }

    if (config().get_bool("IMPORTANCE_DECAY_ENABLED")) {
        importanceDecayAgent->setFrequency( 
            config().get_int("IMPORTANCE_DECAY_CYCLE_PERIOD"));
        this->startAgent(importanceDecayAgent);
    }
    if (config().get_bool("ENTITY_EXPERIENCE_ENABLED")) {
        this->entityExperienceAgent->setFrequency(
           config().get_int( "ENTITY_EXPERIENCE_MOMENT_CYCLE_PERIOD" ) );
        this->startAgent(entityExperienceAgent);
    }

    // TODO: This should be done only after NetworkElement is initialized
    // (i.e., handshake with router is done)
    // Send SUCCESS_LOAD to PROXY, so that it can start sending perception messages
    char str[100];
    sprintf(str, "SUCCESS LOAD %s %s", myId.c_str(), PerceptionActionInterface::PAIUtils::getExternalId(petId.c_str()).c_str());
    StringMessage successLoad(myId, config().get("PROXY_ID"), str);
    logger().info("OAC spawned. Acking requestor");
    if (!sendMessage(successLoad)) {
        logger().error("Could not send SUCCESS LOAD to PROXY!");
        exit(-1);
    }

    if ( config().get("VISUAL_DEBUGGER_ACTIVE") == "true" ) {
        int minPort = boost::lexical_cast<unsigned int>
            ( config().get("MIN_OAC_PORT") );

        std::string visualDebuggerHost = 
            config().get("VISUAL_DEBUGGER_HOST");

        std::string visualDebuggerStartPort =
            config().get("VISUAL_DEBUGGER_PORT");

        unsigned int visualDebuggerPort = static_cast<unsigned int>
            ( boost::lexical_cast<unsigned int>( visualDebuggerStartPort ) + ( portNumber - minPort ) );
        
        this->pet->startVisualDebuggerServer( visualDebuggerHost, 
            boost::lexical_cast<std::string>( visualDebuggerPort ) );

    } // if

}

OAC::~OAC()
{

    // WARNIG: free memory should be implemented if there are more than one opc
    // per process

    delete (planSender);
    delete (petMessageSender);
    delete (predicatesUpdater);
    delete (pai);
    delete (procedureRepository);
    delete (pet);

    // agents
    delete (procedureInterpreterAgent);
    delete (importanceDecayAgent);
    delete (actionSelectionAgent);

#ifndef DELETE_ATOMSPACE 
    // TODO: It takes too much time to delete atomspace. So, atomspace removal
    // is currently disable. This is a hack to allow valgrind tests to work fine. 
    // When atomSpace removal is fast enough, remove this hack and enable delete
    // operation again.
    if (config().get_bool("CHECK_OAC_MEMORY_LEAKS")) {
#endif
    logger().debug("OAC - Starting AtomSpace removal.");
    printf("OAC - Starting AtomSpace removal.\n");
    int t1 = time(NULL);
    delete (atomSpace);
    int t2 = time(NULL);
    logger().debug("OAC - Finished AtomSpace removal. t1 = %d, t2=%d, elapsed time =%d seconds", t1, t2, t2-t1);
    printf("OAC - Finished AtomSpace removal. t1 = %d, t2=%d, diff=%d\n", t1, t2, t2-t1);
#ifndef DELETE_ATOMSPACE 
    }
#endif
}

/* --------------------------------------
 * Private Methods
 * --------------------------------------
 */

void OAC::loadPet(const std::string& petId)
{
    // load pet metadata
    std::string file = getPath(petId, config().get("PET_DUMP"));
    this->pet = Pet::importFromFile(file, petId, atomSpace, petMessageSender);
}

void OAC::loadAtomSpace(const std::string& petId)
{
    // load atom space and other repositories
    std::string file = getPath(petId, config().get("ATOM_SPACE_DUMP"));
    savingLoading.load(file.c_str(), *atomSpace);
}

void OAC::saveState()
{

    if (!createDirectory(getPath(pet->getPetId()).c_str())) {
        logger().error("OAC - Cannot create directory '%s'.",
                     getPath(pet->getPetId()).c_str());
        return;
    }

    // save atom space and othe repositories
    std::string file = getPath(pet->getPetId(), config().get("ATOM_SPACE_DUMP"));
    remove(file.c_str());
    savingLoading.save(file.c_str(), *atomSpace);

    // save pet metadata
    file = getPath(pet->getPetId(), config().get("PET_DUMP"));
    Pet::exportToFile(file, getPet());

    // Pet state saved, send success unload message to proxy
    char str[100];
    sprintf(str, "SUCCESS UNLOAD %s %s", getID().c_str(), PerceptionActionInterface::PAIUtils::getExternalId(pet->getPetId().c_str()).c_str());
    StringMessage successUnload(getID(), config().get("PROXY_ID"), str);
    logger().info("OAC - OAC despawned (state saved). Acking requestor");
    if (!sendMessage(successUnload)) {
        logger().error("Could not send SUCCESS UNLOAD to PROXY!");
    }
}

void OAC::adjustPetToBePersisted()
{

    // drop grabbed object, if any
    if (pet->hasGrabbedObj()) {
        AtomSpaceUtil::setupHoldingObject(*atomSpace, pet->getPetId(), "", pai->getLatestSimWorldTimestamp());
        pet->setGrabbedObj("");
    }

    // put pet in playing mode stopping currently learning process
    if (pet->getMode() == LEARNING) {
        pet->stopLearning(pet->getLearningSchema(), pai->getLatestSimWorldTimestamp());
    }
}

bool OAC::processSpawnerMessage(const std::string & spawnerMessage)
{
    logger().info("OAC::processSpawnerMessage: msg = %s", spawnerMessage.c_str());
    if (spawnerMessage == "SAVE_AND_EXIT") {
        adjustPetToBePersisted();
        saveState();
        logoutFromRouter();
        return true;
    }
    return false;
}

/* --------------------------------------
 * Public Methods
 * --------------------------------------
 */

PerceptionActionInterface::PAI & OAC::getPAI()
{
    return *pai;
}

Pet & OAC::getPet()
{
    return *pet;
}

RuleEngine & OAC::getRuleEngine()
{
    return *ruleEngine;
}

ProcedureInterpreter & OAC::getProcedureInterpreter()
{
    return *procedureInterpreter;
}

ProcedureRepository & OAC::getProcedureRepository()
{
    return *procedureRepository;
}

PVPActionPlanSender & OAC::getPlanSender()
{
    return *planSender;
}

bool OAC::processNextMessage(MessagingSystem::Message *msg)
{
    using namespace combo;

    logger().fine("OAC - Processing next message.");
    bool result;

    // message not for the OAC
    if (msg->getTo() != getID()) {
        logger().warn("OAC - Wrong destination. Message to: %s",
                     msg->getTo().c_str());
        return false;
    }

    // message from petaverse proxy - send to PAI
    if (msg->getFrom() == config().get("PROXY_ID")) {
        HandleSeq toUpdateHandles;
        result = pai->processPVPMessage(msg->getPlainTextRepresentation(), toUpdateHandles);

        if (!result) {
            logger().error("OAC - Unable to process XML message.");
        } else {

            // PVP message processed, update predicates for the
            // added/updated atoms
            predicatesUpdater->update(toUpdateHandles, pai->getLatestSimWorldTimestamp());
            logger().debug("OAC - Message successfully  processed.");
        }
        return false;
    }

    // message from spawner - probabily a SAVE_AND_EXIT
    if (msg->getFrom() == config().get("SPAWNER_ID")) {
        result = processSpawnerMessage((std::string)msg->getPlainTextRepresentation());

        // Message correctly processed, just exit
        if (result) {
            // TODO: Save status...
            logger().info("OAC - Exiting...");
            return true;
        }
    }

    // message from the combo shell to execute a schema
    if (msg->getFrom() == config().get("COMBO_SHELL_ID")) {
        std::string str(msg->getPlainTextRepresentation());
        logger().error("OAC - Got combo shell msg: '%s'", str.c_str());

        if (str.empty())
            return false; //a timing error, maybe?

        std::stringstream ss(str);
        combo_tree tr;
        ss >> tr;
        ComboProcedure cp("", 0, tr);
        std::vector<vertex> args; //an expression, not a function - no args
        procedureInterpreter->runProcedure(cp, args);
        logger().error(
                     "OAC - Called runProcedure(" + ss.str() + ")");
    }

    // message from learning server
    if (msg->getFrom() == config().get("LS_ID")) {
        LearningServerMessages::SchemaMessage * sm = (LearningServerMessages::SchemaMessage *)msg;
        logger().debug("OAC - Got msg from LS: '%s'", msg->getPlainTextRepresentation());

        // sanity check to see if LS does not return an empty
        // ComboSchema
        if (sm->getComboSchema().empty()) {

            logger().warn(
                         "OAC - Received an empty ComboSchema fom LS. Discarding it.");
            return false;

        } else {

            // add schema to combo repository
            //check first if a procedure of that name already exists an remove it
            //if so
            //WARNING : if there was dependencies involving that procedure
            //then removing it is going to generate an invalid procedure_call pointer
            if (procedureRepository->contains(sm->getSchemaName()))
                procedureRepository->remove(sm->getSchemaName());

            //note that if the type is infered and checked
            //it doesn't matter what arity is specified (here 0)
            //because it is going to be overwrite with at the type check
            bool tc = config().get_bool("TYPE_CHECK_LOADING_PROCEDURES");
            arity_t a = infer_arity(sm->getComboSchema());
            procedureRepository->add(ComboProcedure(sm->getSchemaName(),
                                                    a, sm->getComboSchema(),
                                                    tc));
        }

        switch (sm->getType()) {
            // note: assuming arity==0 for now - Moshe

        case MessagingSystem::Message::SCHEMA: {
            // learning is finished, set pet to PLAYING state. This
            // design ensure that the learning info will not be lost
            // until a learned schema is received
            // NOTE: transfered back to when "stop learning" instruction
            // is processed so that OAC does not stay in Lerning mode
            // forever if LS crashes...
            //pet->setMode(PLAYING);

            // Add schema to RuleEngine learned schemata
            ruleEngine->addLearnedSchema( sm->getSchemaName( ) );
        }
        break;

        case MessagingSystem::Message::CANDIDATE_SCHEMA: {
            // Add schema to RuleEngine learned schemata ...
            ruleEngine->addLearnedSchema( sm->getSchemaName( ) );

            // .. and execute it
            pet->setTriedSchema(sm->getSchemaName());
            ruleEngine->tryExecuteSchema( sm->getSchemaName( ) );

        }
        break;

        default: {
            logger().error(
                         "Not a SCHEMA or CANDIDATE_SCHEMA message!!!");
        }
        break;
        }
    }
    return false;
}

void OAC::schemaSelection()
{
    logger().fine("OAC - Executing selectSchemaToExecute().");

    this->pet->getCurrentModeHandler( ).update( );
    this->ruleEngine->processNextAction( );
    this->ruleEngine->runSchemaForCurrentAction( );

//  if ( pet->getMode( ) != PLAYING && pet->getMode( ) != LEARNING ) {
//    pet->setMode( PLAYING );
//  } // if
}

void OAC::decayShortTermImportance()
{
    atomSpace->decayShortTermImportance();
}


/* TODO: OAC does not extend NetworkElement anymore.
 * So, a better design must be created to figure out LS has just recovered from a failure:
void OAC::markAsAvailableElement(const std::string& id){
    logger().debug("OAC - markAsAvailableElement(%s).", id.c_str());

    // remove element from unavailable list and handshake if router
    NetworkElement::markAsAvailableElement(id);

    if (isInitialized()) {
        // OAC-specific actions
        PetMode mode = pet->getMode();
        if(mode == OperationalAvatarController::LEARNING){
            // Pet still in learning mode and LS has recovered from a crash. Restart
            // the learning process from the begining
     // TODO: Review this. Perhaps the best thing to do is exiting Learning mode and give feedback to the user (via Feedback messages)
            if(id == config().get("LS_ID")){
                pet->restartLearning();
            }
        }
    }
}
*/

const std::string OAC::getPath(const std::string& petId, const std::string& filename)
{
    std::string path;

    std::string base = config().get("PET_DATABASE");
    expandPath(base);

    logger().debug("OAC - Pet database directory: %s", base.c_str());
    path.append(base);
    path.append("/pet_");
    path.append(petId);

    // no empty string
    if (filename.size() > 0) {
        path.append("/");
        path.append(filename);
    }

    logger().debug("OAC - getPath: " + path);

    return path;
}