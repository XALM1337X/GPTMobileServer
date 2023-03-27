#include "CoreServer.h"
#include "BPLog.h"
#include "BPStrings.h"
#include "BPFile.h"
#include "BPExec.h"
#include "BPHttp.h"
#include "rapidjson/document.h"
#include <string>
#include <regex>
#include <fstream>
#include <iostream>
#include <cstring>
#include <thread>
#include <vector>

CoreServer::CoreServer() {
    std::cout << "CoreServer::CoreServer: Initializing..." << std::endl;
    BPMainLog::WriteLog("CoreServer::CoreServer: Initializing...","/root/GPTMobileServer/src/logs/MainLog.log");
    this->isRunning = false;
    this->debug_mode = false;
    this->current_command ="";
    this->api_key = "";
    this->Init();
}

CoreServer::~CoreServer() {
    std::cout << "CoreServer::~CoreServer: Shutting Down..." << std::endl;
    BPMainLog::WriteLog("CoreServer::~CoreServer: Shutting Down...","/root/GPTMobileServer/src/logs/MainLog.log");
}

void CoreServer::Init() {
    if (!this->ReadAPIKey()){
        return;
    }    
    this->server_socket = socket(AF_INET, SOCK_STREAM, 0);
    this->opt = 1;
    setsockopt(this->server_socket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &this->opt, sizeof(this->opt));    
    this->address.sin_family = AF_INET;
    this->address.sin_addr.s_addr = INADDR_ANY;
    this->address.sin_port = htons(8080);    
    bind(this->server_socket, (struct sockaddr *)&this->address, sizeof(this->address));
    listen(this->server_socket, 3);
    this->isRunning = true;
    this->StartAcceptHandler();    
}

void CoreServer::StartAcceptHandler() {
    std::thread accept_thread([this]() {
        this->AcceptHandler();
    });
    accept_thread.detach();
}

void CoreServer::StopAcceptHandler() {
    this->isRunning = false;
}

void CoreServer::ToggleDebugMode() {
    if (this->debug_mode) {
        this->debug_mode = false;
    } else {
        this->debug_mode = true;
    }
}

void CoreServer::AcceptHandler() {    
    BPMainLog::WriteLog("Starting Accept Handler","/root/GPTMobileServer/src/logs/MainLog.log");
    int addrlen = sizeof(this->address);
    int new_socket;
    while (this->isRunning) {
        new_socket = accept(this->server_socket, (struct sockaddr *)&this->address, (socklen_t *)&addrlen);
        if (new_socket < 0) {
            BPErrorLog::WriteLog("CoreServer::AcceptHandler:error - Error accepting new connection","/root/GPTMobileServer/src/logs/ErrorLog.log");
            std::cout << "Error accepting new connection" << std::endl;;
            continue;
        }
        std::thread handler_thread([this, new_socket]() {
           this->LaunchHandlerInternals(new_socket);
        });
        handler_thread.detach();
    }
}

void CoreServer::LaunchHandlerInternals(int socket) {
    //TODO: Replace all send() functions data to HTTP messages
    char buffer[(1024*1024*5)] = {0};
    while(this->isRunning) {
        int valread = recv(socket, buffer, (1024*1024*5), 0);
        if (valread == -1) {
            BPErrorLog::WriteLog("CoreServer::LaunchHandlerThread[Lambda:handler_thread]:error - Socket received error. Closing socket: "+std::to_string(socket),"/root/GPTMobileServer/src/logs/ErrorLog.log");
            break;
        } else if (valread == 0) {
            BPMainLog::WriteLog("CoreServer::LaunchHandlerThread[Lambda:handler_thread]: - Client has disconnected from socket, shutting down connection...","/root/GPTMobileServer/src/logs/MainLog.log");
            break;
        } else {
            std::string msg_str(buffer);
            std::string result = this->HandleMessage(msg_str, socket);
            if (result != "") {
                if (result == "server-shutdown-procedure") {
                    send(socket, std::string("").c_str(), strlen(std::string("").c_str()), 0);
                    this->StopAcceptHandler();
                } else if (result == "debug-mode-toggle") {
                    std::cout << std::to_string(this->debug_mode) << std::endl;
                    send(socket, std::string("Debug mode toggled.").c_str(), strlen(std::string("Debug mode toggled.").c_str()), 0);
                } else {
                    int bytesSent = send(socket, result.c_str(), strlen(result.c_str()), 0);
                    if (bytesSent < 0) {
                        BPErrorLog::WriteLog("CoreServer::LaunchHandlerThread[Lambda:handler_thread]:error - Failed to send message to client socket, shuttdown down connection...","/root/GPTMobileServer/src/logs/ErrorLog.log");
                        break;
                    } else {
                        if (this->debug_mode) {
                            std::cout << "Response Sent:" <<std::endl;
                        }                                
                    }   
                }
            } else {
                BPErrorLog::WriteLog("CoreServer::LaunchHandlerThread:error - HandleMessage failed.","/root/GPTMobileServer/src/logs/ErrorLog.log");
                if (this->debug_mode) {
                    std::cout << buffer << std::endl;
                }                        
                std::string err_str = "Internal server error";
                send(socket, err_str.c_str(), strlen(err_str.c_str()), 0);
            }
        }
        memset(buffer, 0, sizeof(buffer));
    }
    close(socket); 
}

std::string CoreServer::HandleMessage(std::string msg, int new_socket) {
    std::string ret = "";
    std::string pattern = "(POST|GET|PUT|HEAD|DELETE|CONNECT|OPTIONS|TRACE)\\s+([a-zA-Z\\/.-]+)\\s+([a-zA-Z]+)\\/([0-9.]+)$";
    std::regex regex(pattern);
    std::smatch match;
    std::vector<std::string> splt_str = BPStrings::SplitString(msg,'\n');    
    if (msg == "--shutdown-server") {
        BPMainLog::WriteLog("CoreServer::HandleMessage:info - Shutting Down","/root/GPTMobileServer/src/logs/MainLog.log");
        if (this->debug_mode) {
            std::cout << "shutting down" << std::endl;
        }        
        ret = "server-shutdown-procedure";
    } else if(msg == "--debug-toggle") {
        ret = "debug-mode-toggle";
        this->ToggleDebugMode();
    } else if(msg == "--help") {
        return this->GetHelp();
    } else if (std::regex_match(splt_str[0], match, regex)) {
        return this->HandleHTTPRequest(splt_str);        
    } else {   
        std::string esc_msg = BPStrings::EscapeStringCharacters(msg);
        this->SetCommand(esc_msg);
        ret = this->ExecuteGPTCommand();
    } 
    return ret;
}

std::string CoreServer::HandleHTTPRequest(std::vector<std::string> lines) {
    BPHttpMessage msg;
    msg.Parse(lines);
    for (auto it = msg.header_map.begin(); it != msg.header_map.end(); ++it) {
        
    }
    
    
    return msg.body;
}

void CoreServer::SetCommand(std::string command_str) {
    std::string command =  "curl -s https://api.openai.com/v1/chat/completions " 
                           "-H \"Authorization: Bearer "+this->api_key+"\" " 
                           "-H \"Content-Type: application/json\" " 
                           "-d '{ \"model\": \"gpt-3.5-turbo\", \"messages\": [{\"role\": \"user\", \"content\": \""+command_str+"\"}]}' ";
    

    this->current_command = command;
}

std::string CoreServer::ExecuteGPTCommand() {    
    std::string ret ="";      
    std::smatch match;
    std::string pattern = "^.*\n+(.*)$";
    std::regex regex(pattern);
    BPExecResult ex = BPExec::Exec(this->GetCommandString());
    if (ex.exit_code != 0) {
        BPErrorLog::WriteLog("Core::ExecuteCommand:error - Failed to execute command.","/root/GPTMobileServer/src/logs/ErrorLog.log");
        return ret;
    }    
    rapidjson::Document document;
    rapidjson::ParseResult result = document.Parse(ex.result.c_str());    
    if (!result || document.HasMember("error")) {
        BPErrorLog::WriteLog("Core::ExecuteCommand:error - Failed parsing JSON","/root/GPTMobileServer/src/logs/ErrorLog.log");
        return "Failed parsing JSON";
    } else {
        const rapidjson::Value& choices = document["choices"];        
        if (choices.IsArray()) {        
            const rapidjson::Value& choice = choices[0];
            if (choice.IsObject()) {        
                const rapidjson::Value& message = choice["message"];        
                if (message.IsObject()) {        
                    const rapidjson::Value& content = message["content"];        
                    if (content.IsString()) {        
                        std::cout << std::endl;
                        std::string str = std::string(content.GetString());
                        if (std::regex_match(str, match, regex)) {        
                            ret = match[1];
                        } else {        
                            ret = str;
                        }
                    }
                }
            }
        }
    } 
    return ret;
}

std::string CoreServer::GetCommandString() {
    return this->current_command;
}

std::string CoreServer::GetHelp() {
    std::string help_str = (
            "DeskGPT:\n"
            "  OPTIONS:\n"
            "    --help or -h       To see this option screen\n"
            "    --exit             To shutdown program\n"
            "    --debug-toggle     Toggle server debug mode\n"
            "    --shutdown-server  Shutdown server\n"
            "    --test-http        Send Test HTTP message to server\n"
            );
    return help_str;
}

bool CoreServer::ReadAPIKey() {
    std::string key = "";
    bool success = BPFile::FileReadString("/root/GPTMobileServer/src/etc/api.key", &key);
    if (!success || key == "") {
        if (key == "") {    
            BPErrorLog::WriteLog("Core::ReadAPIKey:error - api.key file is empty.","/root/GPTMobileServer/src/logs/ErrorLog.log");
        } else {
            BPErrorLog::WriteLog("Core::ReadAPIKey:error - Failed to read api.key file","/root/GPTMobileServer/src/logs/ErrorLog.log");
        }        
        return false;
    }
    this->api_key = key;    
    return true;
}