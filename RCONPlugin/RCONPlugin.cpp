#include "RCONPlugin.h"

#include "utils/parser.h"
#include <iostream>

BAKKESMOD_PLUGIN(RCONPlugin, "RCON plugin", "0.1", PLUGINTYPE_THREADED)

bool RCONPlugin::is_authenticated(connection_ptr hdl) {
	if (auths.find(hdl) == auths.end()) {
		connection_data conData;
		conData.authenticated = false;

		auths.insert(std::pair<connection_ptr, connection_data>(hdl, conData));
	}
	return auths[hdl].authenticated;
}

string stringify(std::vector<std::string> params)
{
	string str = "";
	for (size_t i = 1; i < params.size(); ++i)
	{
		str += params.at(i);
		if (i != params.size() - 1) {
			str += " ";
		}
	}
	str += "";
	return str;
}

void RCONPlugin::on_message(server* s, websocketpp::connection_hdl hdl, message_ptr msg) {
	connection_ptr con = s->get_con_from_hdl(hdl);
	try {
		auto input = parseConsoleInput(msg->get_payload());
		//cvarManager->log(msg->get_payload());
		if (!is_authenticated(con))
		{
			if (input->size() > 0 && input->at(0).size() == 2 && input->at(0).at(0).compare("rcon_password") == 0)
			{
				string authKey = cvarManager->getCvar("rcon_password").getStringValue();
				string userAuthKey = input->at(0).at(1);
				if (!authKey.empty() && !userAuthKey.empty())
				{
					if (authKey.compare(userAuthKey) == 0) {
						auths[con].authenticated = true;
						s->send(hdl, "authyes", msg->get_opcode());
						delete input;
						return;
					}
				}
			}
			auths[con].tries += 1;
			s->send(hdl, "authno", msg->get_opcode());
			delete input;
			return;
		}
		string payload = msg->get_payload();
		gameWrapper->Execute([payload, &_cvarManager = cvarManager, log = *logRcon](GameWrapper* gw) {

			_cvarManager->executeCommand(payload, log);
		});

		delete input;
	}
	catch (const websocketpp::lib::error_code& e) {
		std::cout << "Echo failed because: " << e
			<< "(" << e.message() << ")" << std::endl;
		//cvarManager->log("Websocket error");
	}
}


void RCONPlugin::run_server() {
	try {
		// Set logging settings
		ws_server.set_access_channels(websocketpp::log::alevel::all);
		//ws_server.set_access_channels(websocketpp::log::alevel::access_core);
		//ws_server.set_access_channels(websocketpp::log::alevel::app);
		static std::ofstream wslog("ws.log.txt");
		static std::ofstream wserlog("ws.errlog.txt");
		ws_server.get_alog().set_ostream(&wslog);
		ws_server.get_elog().set_ostream(&wserlog);
		ws_server.get_alog().write(websocketpp::log::alevel::app, "ws_server constructor");
		ws_server.get_elog().write(websocketpp::log::alevel::app, "ws_server constructor elog");
		// Initialize Asio
		ws_server.init_asio();

		// Register our message handler
		ws_server.set_message_handler(bind(&RCONPlugin::on_message, this, &ws_server, ::_1, ::_2));

		// Listen on port 9002
		int port = cvarManager->getCvar("rcon_port").getIntValue();
		ws_server.listen(port);

		// Start the server accept loop
		ws_server.start_accept();

		// Start the ASIO io_service run loop
		ws_server.run();
	}
	catch (websocketpp::exception const & e) {
		std::cout << e.what() << std::endl;
	}
	catch (...) {
		std::cout << "other exception" << std::endl;
	}
}

void RCONPlugin::onLoad()
{
	logRcon = std::make_shared<bool>(false);
	cvarManager->registerCvar("rcon_password", "password");
	cvarManager->registerCvar("rcon_port", "9002"); //Registered in the main dll now
	cvarManager->registerCvar("rcon_timeout", "5");
	cvarManager->registerCvar("rcon_log", "0", "Log all incoming rcon commands", true, true, 0, true, 1, true).bindTo(logRcon);
	cvarManager->registerNotifier("rcon_disconnect", [this](vector<string> commands) {
		ws_server.stop_listening();
		for (auth_iter iterator = auths.begin(); iterator != auths.end(); iterator++)
		{
			if (iterator->first.get()->get_state() == websocketpp::session::state::open)
			{
				iterator->first->close(1000, "Shutting down BM");
				cvarManager->log("Closing websocket connection");
			}
		}
		ws_server.stop();
	}, "Disconnects all rcon connections", PERMISSION_ALL);
	cvarManager->registerNotifier("sendback", [this](vector<string> commands) {
		if (commands.size() > 1)
		{
			for (auth_iter iterator = auths.begin(); iterator != auths.end(); iterator++)
			{
				if (is_authenticated(iterator->first) && iterator->first.get()->get_state() == websocketpp::session::state::open)
				{
					iterator->first->send(stringify(commands));
				}
			}
		}
	}, "Sends text to all connected clients. Usage: sendback abc def ghi \"hij klm\"", PERMISSION_ALL); 

	cvarManager->registerNotifier("ws_inventory", [this](vector<string> commands) {
		if (commands.size() < 3)
			return;
		cvarManager->log("Sending invent");
		for (auth_iter iterator = auths.begin(); iterator != auths.end(); iterator++)
		{
			if (is_authenticated(iterator->first) && iterator->first.get()->get_state() == websocketpp::session::state::open)
			{

				std::string str = "invent_dump";
				for (int i = 1; i < 3; i++)
				{
					str += " \"" + commands.at(i) + "\"";
				}
				cvarManager->executeCommand(str);
				std::string format = commands.at(2).compare("csv") == 0 ? "csv" : "json"; //Dont want any path injection stuff
				std::ifstream ifs("./bakkesmod/data/inventory." + format);
				std::string content((std::istreambuf_iterator<char>(ifs)),
					(std::istreambuf_iterator<char>()));


				string::size_type pos = 0; // Must initialize
				std::stringstream a;
				a << std::endl;
				std::string endl = a.str();
				while ((pos = content.find(endl, pos)) != string::npos)
				{
					content.erase(pos, endl.size());
				}
				auto ec = iterator->first->send(content + "\n", websocketpp::frame::opcode::binary);

				cvarManager->log(ec.message());
			}
		}

	}, "Sends text to all connected clients. Usage: sendback abc def ghi \"hij klm\"", PERMISSION_ALL);
	//thread t(run_server);
	run_server();
	//ws_server.run();
}

void RCONPlugin::onUnload()
{
	if (!ws_server.stopped()) {
		ws_server.stop_listening();
		for (auth_iter iterator = auths.begin(); iterator != auths.end(); )
		{
			if (iterator->first.get()->get_state() == websocketpp::session::state::open)
			{
				iterator->first->close(1000, "Shutting down BM");
				iterator->first->terminate(make_error_code(websocketpp::error::value::general));
			}
			iterator = auths.erase(iterator);
		}
		ws_server.stop();
	}
}
