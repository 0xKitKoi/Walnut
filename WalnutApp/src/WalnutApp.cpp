#include "Walnut/Application.h"
#include "Walnut/EntryPoint.h"

#include "Walnut/Image.h"
#include <thread>
#include "rs232.h" // Cross platform wrapper for USB Serial Com. https://github.com/Marzac/rs232

#include <stdio.h>
#pragma comment(lib, "Ws2_32.lib")

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <fstream>
#include <sstream>



// Global variables
std::queue<std::string> messageQueue; // Shared buffer for messages
std::mutex queueMutex; // Mutex to protect the shared buffer
std::condition_variable queueCV; // Condition variable for signaling
int globalport; // m_SelectedPort isnt accessible when file->close is called. Cherno's gonna hit me over the head with a chair for this
bool stopThreads = false; // Flag to signal threads to stop
bool showPopup = false;
std::string Out; // dump buffer received from server to user in textbox.
bool OpenSettings = false;
SOCKET ConnectSocket; // Global socket shared amongst send and recv threads. I know, its horrible :(
sockaddr_in clientService;
char ipbuf[20] = { 0 };
char portbuf[6] = { 0 };
bool connectionAquired = false;


struct SaveData { // Default settings. Change to microcontroller IP:PORT
	std::string ipAddress = "127.0.0.1";
	int port = 5000;

}m_SaveData;


DWORD WINAPI receiveThread(LPVOID lpParam) {
	SOCKET clientSocket = reinterpret_cast<SOCKET>(lpParam);
	char buffer[1024];
	int bytesReceived;
	while (!stopThreads) {
		bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
		if (bytesReceived > 0) {
			buffer[bytesReceived] = '\0'; // Null-terminate the received data
			std::lock_guard<std::mutex> lock(queueMutex);
			messageQueue.push(std::string(buffer));
			queueCV.notify_one();
			if (connectionAquired) {Out.append("\n [+] " + std::string(buffer));}
			
		}
		else {
			break;
		}
	}
	return 0;
}

DWORD WINAPI sendThread(LPVOID lpParam) {
	SOCKET clientSocket = reinterpret_cast<SOCKET>(lpParam);
	while (!stopThreads) {
		std::unique_lock<std::mutex> lock(queueMutex);
		queueCV.wait(lock, [] { return !messageQueue.empty() || stopThreads; });
		if (stopThreads) break;
		std::string message = messageQueue.front();
		messageQueue.pop();
		lock.unlock();
		send(clientSocket, message.c_str(), message.size(), 0);
		printf("Sent: %s\n", message.c_str());
		
	}
	return 0;
}

void AttemptConnect(SaveData data) {
	ConnectSocket = socket(AF_INET, SOCK_STREAM, 0);
	clientService.sin_family = AF_INET;
	clientService.sin_addr.s_addr = inet_addr(data.ipAddress.c_str());
	clientService.sin_port = htons(data.port);
	if (connect(ConnectSocket, (SOCKADDR*)&clientService, sizeof(clientService)) == SOCKET_ERROR) {

		int error_code = WSAGetLastError();
		printf("Unable to connect to server.%d\n", error_code);
		WSACleanup();
	}
	send(ConnectSocket, "W", 1, 0);
}

void OpenSettingsFile() {
	static char buffer[1024 * 16]; // Buffer to store file contents

	static std::string filePath;
	ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
	ImGui::OpenPopup("Text Editor");

	if (ImGui::BeginPopupModal("Text Editor", &OpenSettings, ImGuiWindowFlags_NoTitleBar)) {
		
		ImGui::Text("Current Settings:");
		// Open file dialog and load selected file into buffer
		std::string selectedFilePath = "Settings.txt";
		if (!selectedFilePath.empty()) {
			filePath = selectedFilePath;
			std::ifstream file(filePath);
			if (file) {
				std::stringstream bufferStream;
				bufferStream << file.rdbuf();
				std::string fileContents = bufferStream.str();
				strncpy_s(buffer, fileContents.c_str(), sizeof(buffer));
				file.close();
				ImGui::Text(fileContents.c_str());
			}
		}
	}
		
	ImGui::Text("EDIT Settings:");
	ImGui::InputText("##Textbox1", ipbuf, sizeof(ipbuf));
	ImGui::InputText("##Textbox2:", portbuf, sizeof(portbuf));
	if (ImGui::Button("Save")) {
		// Save buffer contents to file
		std::ofstream file(filePath);
		if (file) {
			SaveData data;
			data.ipAddress = std::string(ipbuf);
			data.port = atoi(portbuf);
			AttemptConnect(data);
			file << ipbuf << "\n" << portbuf;
			file.close();
		}
	}

	ImGui::SameLine();
	if (ImGui::Button("Close")) {
		ImGui::CloseCurrentPopup();
		OpenSettings = false;	
	}

	ImGui::EndPopup();
}


void OpenAbout() {
	ImGui::SetNextWindowSize(ImVec2(1000, 1000), ImGuiCond_FirstUseEver);
	ImGui::OpenPopup("Popup Window");
	
	if (ImGui::BeginPopupModal("Popup Window", &showPopup, /*ImGuiWindowFlags_NoResize |*/ ImGuiWindowFlags_NoTitleBar)) {
		ImGui::Text("This Program is intended to Send (and recieve) data to a Microcontroller \nVia raw WiFi sockets or through USB Serial Port.");
		ImGui::Text("Toggle Between Network Mode and Serial Mode.");
		ImGui::Text("Network Mode would send data to a IP:PORT set in the settings.");
		ImGui::Text("Serial Mode would send data to serial ports selected in the dropdown menu.");
		ImGui::Text("Mouse Mode sends X and Y coordinates\n Buffer looks like: '\\X(int)Y(int)' for the microcontroller to parse.");
		}
	ImGui::NewLine();
	if (ImGui::Button("Close")) {
		ImGui::CloseCurrentPopup();
		showPopup = false;
	}

	ImGui::EndPopup();
}


class ExampleLayer : public Walnut::Layer
{
public:
	// I think each frame is messing with buffer contents.
	// Something is populating my buffers with non ascii printable chars.
	// microcontroller's fault?
	ExampleLayer() {
		buf = new unsigned char[128]; // seperate buffer, this hopefully wont get rewritten to every single frame
		m_UserInput = new char[128]; // ImGUI::TextBox input. 
		// I can tell my buffer management needs work. TODO: make buffer class!!!
	}
	~ExampleLayer() {
		delete[] buf;
		delete[] m_UserInput;
		stopThreads = true;

		// Notify the sending thread to wake up and check the stopThreads flag
		queueCV.notify_one();

		// Join the threads
		//hReadThread.join();
		//hSendThread.join();

		WSACleanup();
		printf("Done.");
	}


	virtual void OnAttach() override {
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
			printf( "WSAStartup failed\n");
			//return 1;
		}


		// read settings from file.
		std::ifstream m_SettingsFile("Settings.txt");
		if (m_SettingsFile.is_open()) {
			std::string line;
			for (int i = 0; i < 2; i++) {
				std::getline(m_SettingsFile, line);
				if (i == 1) {
					m_SaveData.port = std::stoi(line);
				}
				else if (i == 0) {
					m_SaveData.ipAddress = line;
				}
			}
			m_SettingsFile.close();
		}
		else { 
			// default settings
			std::ofstream file("Settings.txt");
			if (file.is_open()) {
				file << m_SaveData.ipAddress << std::endl;
				file << m_SaveData.port << std::endl;
				file.close();
			}
		}
		
		// Setup Sockets. Attempts of connection will happen by button.
		ConnectSocket = socket(AF_INET, SOCK_STREAM, 0);
		clientService.sin_family = AF_INET;
		clientService.sin_addr.s_addr = inet_addr(m_SaveData.ipAddress.c_str());
		clientService.sin_port = htons(m_SaveData.port);


		m_Image = std::make_shared<Walnut::Image>("Scuzzy.png");

		// Scan for COM ports on Initialization.
		wchar_t lpTargetPath[5000];
		for (int i = 0; i < 255; i++) {
			std::wstring str = L"COM" + std::to_wstring(i); // converting to COM0, COM1, COM2
			DWORD res = QueryDosDevice(str.c_str(), lpTargetPath, 5000);

			// Test the return value and error if any
			if (res != 0) //QueryDosDevice returns zero if it didn't find an object
			{
				m_ComPorts.push_back(i);
				//std::cout << str << ": " << lpTargetPath << std::endl;
			}
			if (::GetLastError() == ERROR_INSUFFICIENT_BUFFER)
			{
			}

		}
	}

	virtual void OnUIRender() override
	{


		ImGui::Begin("Serial Comunication");

		if (OpenSettings) {
			OpenSettingsFile();
		}
		if (showPopup) {
			OpenAbout();
		}
		
		ImGui::SameLine();
		if (!m_MouseMode) {
			if (ImGui::Button("Toggle Mouse Mode")) {
				m_MouseMode = !m_MouseMode;
			}
		}
		else {
			if(ImGui::Button("Text Mode")) {
				m_MouseMode = !m_MouseMode;
			}
		}
		ImGui::SameLine();
		ImGui::PushItemWidth(200);

		if (ImGui::Button("Toggle Serial/Network")) {
			m_NetworkMode = !m_NetworkMode;
			if (m_NetworkMode) {
				int error_code;
				int error_code_size = sizeof(error_code);

				getsockopt(ConnectSocket, SOL_SOCKET, SO_ERROR, (char*)&error_code, &error_code_size);
				if (error_code == SOCKET_ERROR) {
					printf("Connection Attempt failed: %d\n", error_code);
				}
				if (!connectionAquired) {
					if (connect(ConnectSocket, (SOCKADDR*)&clientService, sizeof(clientService)) == SOCKET_ERROR) {

						int error_code = WSAGetLastError();
						printf("Unable to connect to server.%d\n", error_code);
						//WSACleanup();
					}
					else {
						send(ConnectSocket, "W", 1, 0);

						//HANDLE hReadThread = CreateThread(NULL, 0, receiveThread, (LPVOID)ConnectSocket, 0, NULL);
						//HANDLE hSendThread = CreateThread(NULL, 0, sendThread, (LPVOID)ConnectSocket, 0, NULL);
						std::thread recvThread(&receiveThread, static_cast<void*>(&ConnectSocket));
						std::thread sendThread(&sendThread, static_cast<void*>(&ConnectSocket));
						connectionAquired = true;
					}
				}
			}
			else {
				closesocket(ConnectSocket);
			}
		}

		ImGui::SameLine();
		ImGui::PushItemWidth(200);

		
		bool isDropdownOpen = false;
		if (!m_NetworkMode) {
			if (ImGui::BeginCombo("##combo", "Select a Com Port")) {
				int size = m_ComPorts.size();
				for (int i = 0; i < size; i++) {
					bool isSelected = (m_SelectedPort == i);
					int tmp = m_ComPorts.at(i);
					std::string item = std::to_string(tmp);

					if (ImGui::Selectable(item.c_str(), isSelected)) {
						m_SelectedPort = i;
						globalport = i;
						// Attempt to connect to Selected Serial Port.
						char mode[] = { '8','N','1',0 }; // Serial port config. this sets bit mode & parity. 

						if (RS232_OpenComport(m_ComPorts.at(m_SelectedPort) -1 , 115200, mode, 0)) // (ComPort, Baudrate, mode, flowcontrol)
						{
							//printf("Can not open comport COM%i\n", m_ComPorts.at(m_SelectedPort));
							ErrorMsg = "Can not open comport COM" + std::to_string(m_ComPorts.at(m_SelectedPort) - 1);
						}
						printf("Connected to com port: %d", m_ComPorts.at(m_SelectedPort) - 1);
					}
					if (isSelected) {
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
		}


		// Check if the dropdown menu is clicked to open
		if (ImGui::IsItemClicked()) {
			isDropdownOpen = true;

			// Clear the vector of detected ports and scan for more.
			m_ComPorts.clear();
			// Getting list of COM ports super easy
			// https://stackoverflow.com/a/60950058/9274593
			wchar_t lpTargetPath[5000];
			for (int i = 0; i < 255; i++) {
				std::wstring str = L"COM" + std::to_wstring(i); // converting to COM0, COM1, COM2
				DWORD res = QueryDosDevice(str.c_str(), lpTargetPath, 5000);

				// Test the return value and error if any
				if (res != 0) //QueryDosDevice returns zero if it didn't find an object
				{
					m_ComPorts.push_back(i);
					//std::cout << str << ": " << lpTargetPath << std::endl;
				}
				if (::GetLastError() == ERROR_INSUFFICIENT_BUFFER)
				{
					printf("ERROR: %d\n", ::GetLastError());
					ErrorMsg = "ERROR: " + std::to_string(::GetLastError());
				}

			}
		}
		else {
			isDropdownOpen = false;
		}
		
		ImGui::PopItemWidth();
		ImGui::Text(ErrorMsg.c_str());

		

		// Toggles between Sending Mouse Coordinates or Text.
		if (m_MouseMode) {

			ImGui::Text("Hold Left Click over the image to Send coords!");
			ImVec2 imagePos = ImGui::GetCursorScreenPos();
			ImGui::Image(m_Image->GetDescriptorSet(), { 400, 400 });


			ImVec2 pos = ImGui::GetMousePos();
			// Position of mouse cursor relative to image, then scaled by 180 degrees of rotation for the servo motor.
			ImVec2 relativePos = ImVec2((pos.x - imagePos.x) / (m_Image->GetHeight() / 180), (pos.y - imagePos.y) / (m_Image->GetWidth() / 180));
			ImGui::Text("Coords are sent over serial as: (X%uY%u)", (int)relativePos.x, (int)relativePos.y);


			// Basically, you hold left click while cursor is over the image.
			if (ImGui::IsMouseDragging(0)) {
				//printf("Holding mouse button down.\n");
				// Michael's arduino parses this to control two servo motors for Pan And Yaw. Yee Haw.
				//char coords[10];
				std::string temp;
				temp += "\\"; // this is a custom command format, the microcontroller will parse this for the ints.
				temp += "X";
				temp += std::to_string((int)relativePos.x);
				temp += "Y";
				temp += std::to_string((int)relativePos.y);
				temp += "\n";
				temp += '\0';

				unsigned char* charArray = new unsigned char[temp.size()];
				std::memcpy(charArray, temp.c_str(), temp.size());
				// Send Mouse Position over Serial.
				if (!m_NetworkMode) {
					//RS232_SendBuf(m_ComPorts.at(m_SelectedPort) - 1, charArray, temp.size());
					//RS232_cputs(m_ComPorts.at(m_SelectedPort) - 1, temp.c_str());

					int e = RS232_SendBuf(m_ComPorts.at(m_SelectedPort) - 1, charArray, temp.size());
					//RS232_cputs((m_ComPorts.at(m_SelectedPort) - 1), m_UserInput);

					if (e < 0) {
						printf("\nWriting failed! Port: %d", m_ComPorts.at(m_SelectedPort) - 1);
						printf("\n%.*s", temp.size(), m_UserInput);
					}
					printf("\nWrote %d bytes (%s) on Comport %d", e , temp, m_ComPorts.at(m_SelectedPort) - 1);
				}
				else {
					// Send over Network:
					std::lock_guard<std::mutex> lock(queueMutex);
					messageQueue.push(temp.c_str());
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Dont Overload the Microcontroller!!!!
			}

		}
		else {	// Send Text to Serial.

			// Read from COM port.
			if (!m_NetworkMode) {
				unsigned char buffer[128];
				int n = RS232_PollComport(m_ComPorts.at(m_SelectedPort) - 1, buffer, 128);

				if (n > 0)
				{
					printf("\nreceived %i bytes: %s\n", n, (char*)buffer);
					m_Out += "\n [+] ";
					for (int i = 0; i < n; i++) { 
						m_Out += buffer[i]; // I think each frame is messing with my buffers.
						// I seperated them so its not constant garbage.
					}
					m_Out += "\n";

					
					Out += m_Out; // This makes me upset. TODO: make that damn buffer class
					
					m_Out = "";
				}
				memset(buffer, 0, sizeof buffer);

			}
			else {
				// RECIEVE THREAD SHOULD POPULATE 

				std::unique_lock<std::mutex> lock(queueMutex); // Make sure no thread is overwriting what i want to read
				//queueCV.wait(lock, [] { return !messageQueue.empty() || stopThreads; }); // Wait for data to be available in the buffer
				std::string message;
					// Check if new data is available
				if (!messageQueue.empty()) {
					// Get the message from the buffer
					message = messageQueue.front();
					messageQueue.pop();
				}
				queueMutex.unlock();
				//m_Out = Out;
				

			}
			
			ImGui::Text("Serial Comunication.\tCurrent Mode: ");
			if (m_NetworkMode) {
				ImGui::SameLine();
				ImGui::Text("Network Mode");
			}
			else {
				ImGui::SameLine();
				ImGui::Text("Serial Mode!");
			}
			ImGui::Text("Input:");
			//ImGui::InputText("##Text", m_UserInput, IM_ARRAYSIZE(m_UserInput));
			ImGui::InputText("##Text", m_UserInput, 128); // Set limit for buffer debug purposes
			//printf("\n%.*s", 128, m_UserInput);
			ImGui::SameLine();
			if (ImGui::Button("Send")) {
				int i;
				for (i = 0; i < 128; i++) {
					if (m_UserInput[i] == '\0') { // find index of end of null terminated string
						printf("\nFound \\0 at end, %d", i);
						break;
					}
					if (m_UserInput[i] == '\n') {
						printf("\nFound \\n at end, %d", i);
						break;
					}
				}
				m_UserInput[i] = '\n';
				m_UserInput[i + 1] = '\0';
				printf("\n%d byte DUMP:", i);
				printf("\n%.*s", i, m_UserInput);
				// We have user input, send over user selected protocol
				if (!m_NetworkMode) {
					// Write to COM port.
					int e = RS232_SendBuf(m_ComPorts.at(m_SelectedPort) - 1, reinterpret_cast<unsigned char*>(m_UserInput), i+1);
					//RS232_cputs((m_ComPorts.at(m_SelectedPort) - 1), m_UserInput);
					
					if (e < 0) {
						printf("\nWriting failed! Port: %d", m_ComPorts.at(m_SelectedPort) - 1);
						printf("\n%.*s", i, m_UserInput);
					}
					printf("\nWrote %d bytes on Comport %d", e, m_ComPorts.at(m_SelectedPort) - 1);
					
				}
				else {
					// TODO: Send Text to Server.
					//send(ConnectSocket, m_UserInput, i, 0);
					std::lock_guard<std::mutex> lock(queueMutex);
					messageQueue.push(m_UserInput);

				}
				memset(m_UserInput, 0, sizeof(m_UserInput)); // clear the user input buffer after sending.
			}

			ImGui::SameLine();
			if (ImGui::Button("Clear Buffer")) {
				Out = ""; // This is the "console window" in Text Mode.
				memset(m_UserInput, 0, sizeof(m_UserInput)); // IDK why but sometimes i have garbage chars in here. 
			}
			ImGui::BeginChild("##Text Box", ImVec2(400, 300), true, ImGuiWindowFlags_NoScrollbar);
			ImGui::Text(Out.c_str()); 
			ImGui::EndChild();

		}

		ImGui::End();

	}

public:
	static char* m_UserInput; // Obligatory buffer for user input.
private:

	std::vector<int> m_ComPorts; // Dynamic array of available COM ports.
	bool m_AboutModalOpen = false;
	bool m_MouseMode = false; // Toggles Mouse mode.
	bool m_NetworkMode = false; // Switches between serial and network mode.
	std::shared_ptr<Walnut::Image> m_Image; // When playing with Walnut, i had an AI gen'd image of Giygas from Earthbound. This is it.
	static std::string m_Out; // This is the backgroud buffer that will eventually get written to the forward buffer on screen.
	std::string ErrorMsg; // For telling the user they probably selected the wrong com port. could be a popup? annoying
	int m_SelectedPort = 0;
	unsigned char* buf; // Buffer for incoming data from serial port.
	
	std::ifstream m_SettingsFile;

};

// Shameful Global variables. Unresolved external symbol error happens if I don't have this here. There's probably a better way.

char* ExampleLayer::m_UserInput = nullptr;
std::string ExampleLayer::m_Out; // Cherno would smite me for this.
//int globalport; // m_SelectedPort isnt accessible when file->close is called. Cherno's gonna hit me over the head with a chair for this



Walnut::Application* Walnut::CreateApplication(int argc, char** argv)
{
	Walnut::ApplicationSpecification spec;
	spec.Name = "Serial Comunication Walnut App";

	Walnut::Application* app = new Walnut::Application(spec);
	app->PushLayer<ExampleLayer>();
	app->SetMenubarCallback([app]()
	{
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("Exit"))
			{
				app->Close();
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit")) {
			

			if (ImGui::MenuItem("Settings")) {
				OpenSettings = true;
			}
			if (ImGui::MenuItem("About")) {
				showPopup = true;
			}

			ImGui::EndMenu();

		}
	});
	return app;
}