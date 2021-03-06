// --------------------------------------------------------------------------------------------------
//  Copyright (c) 2016 Microsoft Corporation
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
//  associated documentation files (the "Software"), to deal in the Software without restriction,
//  including without limitation the rights to use, copy, modify, merge, publish, distribute,
//  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//  
//  The above copyright notice and this permission notice shall be included in all copies or
//  substantial portions of the Software.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
//  NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
//  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
//  DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// --------------------------------------------------------------------------------------------------

// Header: 
#include "AgentHost.h"

// Local:
#include "FindSchemaFile.h"
#include "TCPClient.h"
#include "WorldState.h"

// Boost:
#include <boost/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/utility/in_place_factory.hpp>
#include <boost/make_shared.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/regex.hpp>

// Schemas:
#include <MissionEnded.h>

// STL:
#include <exception>
#include <sstream>
#include <regex>
#include <mutex>

namespace malmo
{
    std::once_flag test_schemas_flag;

    // set defaults
    AgentHost::AgentHost()
        : ArgumentParser( std::string("Malmo version: ") + BOOST_PP_STRINGIZE(MALMO_VERSION) )
        , video_policy(LATEST_FRAME_ONLY)
        , rewards_policy(SUM_REWARDS)
        , observations_policy(LATEST_OBSERVATION_ONLY)
        , current_role( 0 )
        , display_client_pool_messages( false )
    {
        xercesc::XMLPlatformUtils::Initialize();
        std::call_once(test_schemas_flag, testSchemasCompatible);

        this->addOptionalFlag("help,h", "show description of allowed options");
        this->addOptionalFlag("test",   "run this as an integration test");

        // start the io_service on background threads
        this->work = boost::in_place(boost::ref(this->io_service));
        const int NUM_BACKGROUND_THREADS = 1; // can be increased if I/O becomes a bottleneck
        for( int i = 0; i < NUM_BACKGROUND_THREADS; i++ )
            this->background_threads.push_back( boost::make_shared<boost::thread>( boost::bind( &boost::asio::io_service::run, &this->io_service ) ) );
    }

    AgentHost::~AgentHost()
    {
        this->work = boost::none;
        this->io_service.stop();
        for( auto& t : this->background_threads )
            t->join();
        this->close();

        xercesc::XMLPlatformUtils::Terminate();
    }

    void AgentHost::testSchemasCompatible()
    {
        // Attempt to find each of the required schemas and check their version number
        // against the version number we were built with.
        std::string targetVersion = BOOST_PP_STRINGIZE(MALMO_VERSION);
        // Version number should be in the form MAJOR.MINOR.PATCH; we only care about the major and minor numbers.
        std::vector<std::string> parts;
        boost::split(parts, targetVersion, [](char c){ return c == '.'; });
        if (parts.size() != 3)
            throw std::runtime_error("Malformed version number - check root CMakeLists.txt. MALMO_VERSION should be in form MAJOR.MINOR.PATCH - instead we got " + targetVersion + ".");
        targetVersion = parts[0] + "." + parts[1];

        std::vector<std::string> schemas = { "Mission.xsd", "MissionInit.xsd", "MissionEnded.xsd", "MissionHandlers.xsd", "Types.xsd" };
        for (auto it : schemas)
        {
            std::string xsdVersion = extractVersionNumber(it);
            if (xsdVersion != targetVersion)
                throw std::runtime_error("Schema " + it + " has the wrong version number - should be " + targetVersion + " but we got " + xsdVersion + ". Check that MALMO_XSD_PATH is correct.");
        }
    }

    std::string AgentHost::extractVersionNumber(std::string name)
    {
        // Doesn't seem to be a way to use CodeSynthesis to parse the xsd itself, and we don't
        // want to introduce another dependency by using a dedicated xml parsing library, so we
        // extract the version number ourselves using a good old fashioned regex.
        boost::regex re{ ".*<xs:schema.*(?!jaxb:).{5}version=\"([0-9.]*)\"" }; // Find the first version number after the <xs:schema opening tag, taking care to ignore the jaxb:version number.
        std::ifstream stream(FindSchemaFile(name));
        std::string xml = "";
        std::string line = "";
        std::string version = "";
        // Keep concatenating lines until we have a match, or we run out of schema.
        while (version.empty() && getline(stream, line))
        {
            boost::trim(line);
            xml += line;
            boost::smatch matches;
            if (boost::regex_search(xml, matches, re) && matches.size() > 1)
            {
                version = matches.str(1);
            }
        }
        return version;
    }

    void AgentHost::startMission(const MissionSpec& mission, const MissionRecordSpec& mission_record)
    {
        ClientPool client_pool;
        client_pool.add(ClientInfo("127.0.0.1"));

        startMission(mission, client_pool, mission_record, 0, "");
    }

    void AgentHost::startMission(const MissionSpec& mission, const ClientPool& client_pool, const MissionRecordSpec& mission_record, int role, std::string unique_experiment_id)
    {
        if (role < 0 || role >= mission.getNumberOfAgents())
        {
            if (mission.getNumberOfAgents() == 1)
                throw std::runtime_error("Role " + std::to_string(role) + " is invalid for this single-agent mission - must be 0.");
            else
                throw std::runtime_error("Role " + std::to_string(role) + " is invalid for this multi-agent mission - must be in range 0-" + std::to_string(mission.getNumberOfAgents() - 1) + ".");
        }
        if( mission.isVideoRequested( role ) ) 
        {
            if( mission.getVideoWidth( role ) % 4 )
                throw std::runtime_error("Video width must be divisible by 4.");
            if( mission.getVideoHeight( role ) % 2 )
                throw std::runtime_error("Video height must be divisible by 2.");
        }
        
        boost::lock_guard<boost::mutex> scope_guard(this->world_state_mutex);

        if (this->world_state.is_mission_running) {
            throw std::runtime_error("A mission is already running.");
        }

        initializeOurServers( mission, mission_record, role, unique_experiment_id );

        ClientPool pool = client_pool;
        if (mission.getNumberOfAgents() > 1 && role == 0)
        {
            // this is a multi-agent mission and we are the agent responsible for the integrated server.
            // our mission should have been started before any of the others are attempted.
            // This means we are in a position to reserve clients in the client pool:
            ClientPool reservedAgents = reserveClients(client_pool, mission.getNumberOfAgents());
            if (reservedAgents.clients.size() != mission.getNumberOfAgents())
            {
                // Not enough clients available - go no further.
                throw std::runtime_error("There are not enough clients availble in the ClientPool to start this " + std::to_string(mission.getNumberOfAgents()) + " agent mission.");
            }
            pool = reservedAgents;
        }
        if( mission.getNumberOfAgents() > 1 && role > 0 && !this->current_mission_init->hasMinecraftServerInformation())
        {
            // this is a multi-agent mission and we are not the agent that connects to the client with the integrated server
            // so we need to ask the clients in the client pool until we find it
            findServer(pool);
        }
        
        // work through the client pool until we find a client to run our mission for us
        findClient( pool );

        this->world_state.clear();
        // NB. Sets is_mission_running to false. The Mod decides when the mission actually starts (it might need to wait for other agents to join, for example)
        //     and will then send us a MissionInit message, but at this point in time this->world_state->is_mission_running is false.

        if (this->current_mission_record->isRecording()){
            std::ofstream missionInitXML(this->current_mission_record->getMissionInitPath());
            missionInitXML << this->current_mission_init->getAsXML(true);
        }
    }
    
    void AgentHost::initializeOurServers(const MissionSpec& mission, const MissionRecordSpec& mission_record, int role, std::string unique_experiment_id)
    {
        // make a MissionInit structure with default settings
        this->current_mission_init = boost::make_shared<MissionInitSpec>( mission, unique_experiment_id, role );
        
        this->current_mission_record = boost::make_shared<MissionRecord>( mission_record );
        this->current_role = role;

        listenForMissionControlMessages(this->current_mission_init->getAgentMissionControlPort());
        if (mission.isVideoRequested(this->current_role))
        {
            listenForVideo(this->current_mission_init->getAgentVideoPort(),
                mission.getVideoWidth(this->current_role),
                mission.getVideoHeight(this->current_role),
                mission.getVideoChannels(this->current_role));
        }
        listenForRewards(this->current_mission_init->getAgentRewardsPort());
        listenForObservations(this->current_mission_init->getAgentObservationsPort());

        if (this->commands_stream.is_open()){
            this->commands_stream.close();
        }

        if (this->current_mission_record->isRecordingCommands()){
            this->commands_stream.open(this->current_mission_record->getCommandsPath());
        }

        // if the requested port was zero then the system will assign a free one, so store the resulting value in the MissionInit node for sending to the client
        this->current_mission_init->setAgentMissionControlPort(this->mission_control_server->getPort());
        this->current_mission_init->setAgentObservationsPort(this->observations_server->getPort());
        if (this->video_server) {
            this->current_mission_init->setAgentVideoPort(this->video_server->getPort());
        }
        this->current_mission_init->setAgentRewardsPort(this->rewards_server->getPort());
    }
    
    std::string AgentHost::generateMissionInit()
    {
        const bool prettyPrint = false;
        const std::string generated_xml = this->current_mission_init->getAsXML( prettyPrint );

        try {
            const bool validate = true;
            MissionInitSpec test_output(generated_xml,validate);
        }
        catch (xml_schema::exception& e)
        {
            std::ostringstream oss;
            oss << "Internal error: the XML we generate does not validate: " << e.what() << "\n" << e << "\n" << generated_xml << std::endl;
            throw std::runtime_error(oss.str());
        }

        return generated_xml;
    }

    ClientPool AgentHost::reserveClients(const ClientPool& client_pool, int clients_required)
    {
        ClientPool reservedClients;
        std::string reply;
        // TODO - currently reserved for 20 seconds (the 20000 below) - make this configurable.
        std::string request = std::string("MALMO_REQUEST_CLIENT:") + BOOST_PP_STRINGIZE(MALMO_VERSION) + ":20000:" + this->current_mission_init->getExperimentID() + +"\n";
        for (const ClientInfo& item : client_pool.clients)
        {
            if (this->display_client_pool_messages)
                std::cout << "DEBUG: Sending reservation request to " << item.ip_address << " : " << item.port << std::endl;
            try
            {
                reply = SendStringAndGetShortReply(this->io_service, item.ip_address, item.port, request, false);
            }
            catch (std::exception&)
            {
                // This is expected quite often - client is likely not running.
                continue;
            }
            if (this->display_client_pool_messages)
                std::cout << "DEBUG: Reserving client, received reply from " << item.ip_address << ": " << reply << std::endl;

            const std::string malmo_reservation_prefix = "MALMOOK";
            if (reply.find(malmo_reservation_prefix) == 0)
            {
                // Successfully reserved this client.
                reservedClients.add(item);
                clients_required--;
                if (clients_required == 0)
                    break;  // We've got all the clients we need.
            }
        }
        // Were there enough clients available?
        if (clients_required > 0)
        {
            // No - release the clients we already reserved.
            for (const ClientInfo& item : reservedClients.clients)
            {
                if (this->display_client_pool_messages)
                    std::cout << "DEBUG: Cancelling reservation request with " << item.ip_address << " : " << item.port << std::endl;
                try
                {
                    reply = SendStringAndGetShortReply(this->io_service, item.ip_address, item.port, "MALMO_CANCEL_REQUEST\n", false);
                }
                catch (std::exception&)
                {
                    // This is not expected, and probably means something bad has happened.
                    continue;
                }
                if (this->display_client_pool_messages)
                    std::cout << "DEBUG: Cancelling reservation, received reply from " << item.ip_address << ": " << reply << std::endl;
            }
            reservedClients.clients.clear();
        }
        return reservedClients;
    }

    bool AgentHost::findServer(const ClientPool& client_pool)
    {
        std::string reply;
        std::string request = std::string("MALMO_FIND_SERVER") + this->current_mission_init->getExperimentID() + +"\n";
        for (const ClientInfo& item : client_pool.clients)
        {
            if (this->display_client_pool_messages)
                std::cout << "DEBUG: Sending find server request to " << item.ip_address << " : " << item.port << std::endl;
            try
            {
                reply = SendStringAndGetShortReply(this->io_service, item.ip_address, item.port, request, false);
            }
            catch (std::exception&)
            {
                // This is expected quite often - client is likely not running.
                continue;
            }
            if (this->display_client_pool_messages)
                std::cout << "DEBUG: Seeking server, received reply from " << item.ip_address << ": " << reply << std::endl;

            const std::string malmo_server_prefix = "MALMOS";
            if (reply.find(malmo_server_prefix) == 0)
            {
                size_t colon = reply.find_first_of(':');
                if (colon == std::string::npos)
                    throw std::runtime_error("Received malformed reply: " + reply);
                std::string minecraft_server_address = reply.substr(malmo_server_prefix.length(), colon - malmo_server_prefix.length());
                std::string minecraft_server_port_as_string = reply.substr(colon + 1, std::string::npos);
                int minecraft_server_port;
                int n = sscanf(minecraft_server_port_as_string.c_str(), "%d", &minecraft_server_port);
                if (n != 1)
                    throw std::runtime_error("Received malformed reply: " + reply);
                this->current_mission_init->setMinecraftServerInformation(minecraft_server_address, minecraft_server_port);
                return true;
            }
        }

        this->close();
        throw std::runtime_error("Failed to find the server for this mission - you must start the agent that has role 0 first.");
    }

    void AgentHost::findClient(const ClientPool& client_pool)
    {
        std::string reply;
        // As a reasonable optimisation, assume that clients are started in the order of their role, for multi-agent missions.
        // So start looking at position <role> within the client pool.
        // Eg, if the first four agents get clients 1,2,3 and 4 respectively, agent 5 doesn't need to waste time checking
        // the first four clients.
        int num_clients = client_pool.clients.size();
        for (int i = 0; i < num_clients; i++)
        {
            const ClientInfo& item = client_pool.clients[(i + this->current_role) % num_clients];
            this->current_mission_init->setClientAddress( item.ip_address );
            this->current_mission_init->setClientMissionControlPort( item.port );
            const std::string mission_init_xml = generateMissionInit() + "\n";
    
            if (this->display_client_pool_messages)
                std::cout << "DEBUG: Sending MissionInit to " << item.ip_address << " : " << item.port << std::endl;
            try 
            {
                reply = SendStringAndGetShortReply( this->io_service, item.ip_address, item.port, mission_init_xml, false );
            }
            catch( std::exception& ) {
                // This is expected quite often - client is likely not running.
                continue;
            }
            if (this->display_client_pool_messages)
                std::cout << "DEBUG: Looking for client, received reply from " << item.ip_address << ": " << reply << std::endl;
            // this is either a) a single agent mission, b) a multi-agent mission but we are role 0, 
            // or c) a multi-agent mission where we have already located the server
            // expected: MALMOBUSY, MALMOOK, MALMOERROR...
            const std::string malmo_mission_accepted = "MALMOOK";
            if( reply == malmo_mission_accepted )
                return; // mission was accepted, now wait for the mission to start
        }

        this->close();
        throw std::runtime_error( "Failed to find an available client for this mission - tried all the clients in the supplied client pool." );
    }

    WorldState AgentHost::peekWorldState() const
    {
        boost::lock_guard<boost::mutex> scope_guard(this->world_state_mutex);

        return this->world_state;
    }

    WorldState AgentHost::getWorldState()
    {
        boost::lock_guard<boost::mutex> scope_guard(this->world_state_mutex);

        WorldState old_world_state( this->world_state );
        this->world_state.clear();
        this->world_state.is_mission_running = old_world_state.is_mission_running;
        this->world_state.has_mission_begun = old_world_state.has_mission_begun;
        return old_world_state;
    }

    std::string AgentHost::getRecordingTemporaryDirectory() const
    {
        return this->current_mission_record && this->current_mission_record->isRecording() ? this->current_mission_record->getTemporaryDirectory() : "";
    }

    void AgentHost::setDebugOutput(bool debug)
    {
        this->display_client_pool_messages = debug;
    }

    void AgentHost::setVideoPolicy(VideoPolicy videoPolicy)
    {
        this->video_policy = videoPolicy;
    }

    void AgentHost::setRewardsPolicy(RewardsPolicy rewardsPolicy)
    {
        this->rewards_policy = rewardsPolicy;
    }

    void AgentHost::setObservationsPolicy(ObservationsPolicy observationsPolicy)
    {
        this->observations_policy = observationsPolicy;
    }
    
    void AgentHost::listenForMissionControlMessages( int port )
    {
        if( this->mission_control_server && ( port==0 || this->mission_control_server->getPort()==port ) )
        {
            return; // can re-use existing server
        }
        
        this->mission_control_server = boost::make_shared<StringServer>( this->io_service, port, boost::bind( &AgentHost::onMissionControlMessage, this, _1 ) );
        this->mission_control_server->start();
    }
    
    void AgentHost::listenForVideo( int port, short width, short height, short channels )
    {
        if( !this->video_server || 
            (port != 0 && this->video_server->getPort() != port ) ||
            this->video_server->getWidth() != width || 
            this->video_server->getHeight() != height ||
            this->video_server->getChannels() != channels )
        {
            this->video_server = boost::make_shared<VideoServer>( this->io_service, port, width, height, channels, boost::bind(&AgentHost::onVideo, this, _1));

            if (this->current_mission_record->isRecordingMP4()){
                this->video_server->recordMP4(this->current_mission_record->getMP4Path(), this->current_mission_record->getMP4FramesPerSecond(), this->current_mission_record->getMP4BitRate());
            }
            
            this->video_server->start();
        } 
        else {
            // re-use the existing video_server
            // but now we need to re-create the file writers with the new file names
            if (this->current_mission_record->isRecordingMP4()){
                this->video_server->recordMP4(this->current_mission_record->getMP4Path(), this->current_mission_record->getMP4FramesPerSecond(), this->current_mission_record->getMP4BitRate());
            }
        }
        
        this->video_server->startRecording();
    }
    
    void AgentHost::listenForRewards( int port )
    {
        if( !this->rewards_server || ( port != 0 && this->rewards_server->getPort() != port ) )
        {
            this->rewards_server = boost::make_shared<StringServer>(this->io_service, port, boost::bind(&AgentHost::onReward, this, _1));
            this->rewards_server->start();
        }
            
        if (this->current_mission_record->isRecordingRewards()){
            this->rewards_server->record(this->current_mission_record->getRewardsPath());
        }
    }
    
    void AgentHost::listenForObservations( int port )
    {
        if( !this->observations_server || ( port != 0 && this->observations_server->getPort() != port ) ) 
        {
            this->observations_server = boost::make_shared<StringServer>(this->io_service, port, boost::bind(&AgentHost::onObservation, this, _1));
            this->observations_server->start();
        }

        if (this->current_mission_record->isRecordingObservations()){
            this->observations_server->record(this->current_mission_record->getObservationsPath());
        }
    }
    
    void AgentHost::onMissionControlMessage(TimestampedString xml)
    {
        boost::lock_guard<boost::mutex> scope_guard(this->world_state_mutex);

        std::stringstream ss( xml.text );
        boost::property_tree::ptree pt;
        try {
            boost::property_tree::read_xml( ss, pt);
        }
        catch( std::exception&e ) {
            TimestampedString error_message( xml );
            error_message.text = std::string("Error parsing mission control message as XML: ") + e.what() + ":\n" + xml.text.substr(0,20) + "...\n";
            this->world_state.errors.push_back( boost::make_shared<TimestampedString>( error_message ) );
            return;
        }

        if( pt.empty() )
        {
            TimestampedString error_message( xml );
            error_message.text = "Empty XML string in mission control message";
            this->world_state.errors.push_back( boost::make_shared<TimestampedString>( error_message ) );
            return;
        }
        std::string root_node_name(pt.front().first.data());

        if( !this->world_state.is_mission_running && root_node_name == "MissionInit" ) {
            try {
                const bool validate = true;
                this->current_mission_init = boost::make_shared<MissionInitSpec>(xml.text,validate);
                this->world_state.is_mission_running = true;
                this->world_state.has_mission_begun = true;
            }
            catch (const xml_schema::exception& e) {
                std::ostringstream oss;
                oss << "Error parsing MissionInit message XML: " << e.what() << " : " << e << ":" << xml.text.substr(0, 20) << "...";
                TimestampedString error_message(xml);
                error_message.text = oss.str();
                this->world_state.errors.push_back( boost::make_shared<TimestampedString>( error_message ) );
                return;
            }
            this->openCommandsConnection();
        }
        else if( root_node_name == "MissionEnded" ) {
            
            try {
                const bool validate = true;
                
                xml_schema::properties props;
                props.schema_location(xml_namespace, FindSchemaFile("MissionEnded.xsd"));

                xml_schema::flags flags = xml_schema::flags::dont_initialize;
                if( !validate )
                    flags = flags | xml_schema::flags::dont_validate;

                std::istringstream iss(xml.text);
                std::unique_ptr<malmo::schemas::MissionEnded> mission_ended = malmo::schemas::MissionEnded_(iss, flags, props);

                switch( mission_ended->Status() ) {
                    case malmo::schemas::MissionResult::ENDED:
                    case malmo::schemas::MissionResult::PLAYER_DIED:
                        break;
                    default: 
                    {
                        std::ostringstream oss;
                        oss << "Mission ended abnormally: " << mission_ended->HumanReadableStatus();
                        TimestampedString error_message(xml);
                        error_message.text = oss.str();
                        this->world_state.errors.push_back( boost::make_shared<TimestampedString>( error_message ) );
                    }
                    break;
                }
                
                if (this->world_state.is_mission_running) {
                    schemas::MissionEnded::Reward_optional final_reward_optional = mission_ended->Reward();
                    if( final_reward_optional.present() ) {
                        TimestampedReward final_reward(xml.timestamp,final_reward_optional.get());
                        this->processReceivedReward(final_reward);
                        this->rewards_server->recordMessage(TimestampedString(xml.timestamp, final_reward.getAsSimpleString()));
                    }
                }
            }
            catch (const xml_schema::exception& e) {
                std::ostringstream oss;
                oss << "Error parsing MissionEnded message XML: " << e.what() << " : " << e << ":" << xml.text.substr(0, 20) << "...";
                TimestampedString error_message(xml);
                error_message.text = oss.str();
                this->world_state.errors.push_back( boost::make_shared<TimestampedString>( error_message ) );
                return;
            }
            
            this->close();
        }
        else if (root_node_name == "ping") {
            // The mod is pinging us to check we are still around - do nothing.
        }
        else {
            TimestampedString error_message( xml );
            error_message.text = "Unknown mission control message root node or at wrong time: " + root_node_name + " :" + xml.text.substr(0, 200) + "...";
            this->world_state.errors.push_back( boost::make_shared<TimestampedString>( error_message ) );
            return;
        }

        this->world_state.mission_control_messages.push_back( boost::make_shared<TimestampedString>( xml ) );
    }
    
    void AgentHost::openCommandsConnection()
    {
        int mod_commands_port = this->current_mission_init->getClientCommandsPort();
        if( mod_commands_port == 0 ) {
            throw std::runtime_error( "AgentHost::openCommandsConnection : client commands port is unknown! Has the mission started?" );
        }

        std::string mod_address = this->current_mission_init->getClientAddress();

        this->commands_connection = ClientConnection::create( this->io_service, mod_address, mod_commands_port );
    }

    void AgentHost::close()
    {
        this->world_state.is_mission_running = false;
        if (this->video_server) {
            this->video_server->stopRecording();
        }

        if (this->observations_server){
            this->observations_server->stopRecording();
        }

        if (this->rewards_server){
            this->rewards_server->stopRecording();
        }

        if (this->commands_stream.is_open()){
            this->commands_stream.close();
        }
        
        if( this->commands_connection ) {
            this->commands_connection.reset();
        }
    }

    void AgentHost::onVideo(TimestampedVideoFrame message)
    {
        boost::lock_guard<boost::mutex> scope_guard(this->world_state_mutex);

        switch( this->video_policy )
        {
            case VideoPolicy::LATEST_FRAME_ONLY:
                this->world_state.video_frames.clear();
                this->world_state.video_frames.push_back( boost::make_shared<TimestampedVideoFrame>( message ) );
                break;
            case VideoPolicy::KEEP_ALL_FRAMES:
                this->world_state.video_frames.push_back( boost::make_shared<TimestampedVideoFrame>( message ) );
                break;
        }
        
        this->world_state.number_of_video_frames_since_last_state++;
    }
    
    void AgentHost::onReward(TimestampedString message)
    {
        boost::lock_guard<boost::mutex> scope_guard(this->world_state_mutex);
        try {
            TimestampedReward reward;
            reward.createFromSimpleString(message.timestamp, message.text);
            this->processReceivedReward(reward);
        }
        catch (std::exception& e) {
            std::ostringstream oss;
            oss << "Error parsing Reward message: " << e.what() << " : " << message.text;
            TimestampedString error_message(message);
            error_message.text = oss.str();
            this->world_state.errors.push_back(boost::make_shared<TimestampedString>(error_message));
        }
    }
        
    void AgentHost::processReceivedReward( TimestampedReward reward )
    {
        switch( this->rewards_policy )
        {
            case RewardsPolicy::LATEST_REWARD_ONLY:
                this->world_state.rewards.clear();
                this->world_state.rewards.push_back( boost::make_shared<TimestampedReward>( reward ) );
                break;
            case RewardsPolicy::SUM_REWARDS:
                if( !this->world_state.rewards.empty() ) {
                    reward.add(*this->world_state.rewards.front());
                    this->world_state.rewards.clear();
                }
                this->world_state.rewards.push_back( boost::make_shared<TimestampedReward>( reward ) );
                // (timestamp is that of latest reward, even if zero)
                break;
            case RewardsPolicy::KEEP_ALL_REWARDS:
                this->world_state.rewards.push_back( boost::make_shared<TimestampedReward>( reward ) );
                break;
        }
        
        this->world_state.number_of_rewards_since_last_state++;
    }
    
    void AgentHost::onObservation(TimestampedString message)
    {
        boost::lock_guard<boost::mutex> scope_guard(this->world_state_mutex);

        switch( this->observations_policy )
        {
            case ObservationsPolicy::LATEST_OBSERVATION_ONLY:
                this->world_state.observations.clear();
                this->world_state.observations.push_back( boost::make_shared<TimestampedString>( message ) );
                break;
            case ObservationsPolicy::KEEP_ALL_OBSERVATIONS:
                this->world_state.observations.push_back( boost::make_shared<TimestampedString>( message ) );
                break;
        }
        
        this->world_state.number_of_observations_since_last_state++;
    }
    
    void AgentHost::sendCommand(std::string command)
    {
        boost::lock_guard<boost::mutex> scope_guard(this->world_state_mutex);

        if( !this->commands_connection ) {
            TimestampedString error_message(
                boost::posix_time::microsec_clock::universal_time(),
                "AgentHost::sendCommand : commands connection is not open. Is the mission running?"
                );
            this->world_state.errors.push_back( boost::make_shared<TimestampedString>( error_message ) );
            return;
        }

        try {
            this->commands_connection->send(command);
        }
        catch (const std::runtime_error& e) {
            TimestampedString error_message(
                boost::posix_time::microsec_clock::universal_time(),
                "AgentHost::sendCommand : failed to send command: " + std::string(e.what())
                );
            this->world_state.errors.push_back( boost::make_shared<TimestampedString>( error_message ) );
            return;
        }

        if (this->commands_stream.is_open()){
            std::string timestamp = boost::posix_time::to_iso_string(boost::posix_time::microsec_clock::universal_time());
            this->commands_stream << timestamp << " " << command << std::endl;
        }
    }

    boost::shared_ptr<MissionInitSpec> AgentHost::getMissionInit()
    {
        return this->current_mission_init;
    }

    const boost::shared_ptr<MissionInitSpec> AgentHost::getMissionInit() const
    {
        return this->current_mission_init;
    }

    std::ostream& operator<<(std::ostream& os, const AgentHost& agent_host)
    {
        os << "AgentHost";
        if (agent_host.getMissionInit())
            os << ": active (with mission)";
        else
            os << ": uninitialised (no mission init)";
        return os;
    }
}
