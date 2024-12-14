#include <mpi.h>
#include <iostream>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <random>
#include <chrono>
#include <thread>
#include <mutex>


// Structure pour stocker les informations d'un badge
struct BadgeInfo {
    int id;
    std::string name;
    std::vector<int> accessBuildings;
};

// Fonctions utilitaires
std::map<int, BadgeInfo> loadBadgesFromFile(const std::string &filename) {
    std::map<int, BadgeInfo> db;
    std::ifstream file(filename);
    if(!file.is_open()) {
        std::cerr << "Impossible d'ouvrir le fichier " << filename << std::endl;
        return db;
    }
    std::string line;
    while(std::getline(file, line)) {
        if(line.empty()) continue;
        std::stringstream ss(line);
        BadgeInfo b;
        ss >> b.id >> b.name;
        std::string access;
        ss >> access; 
        // access est du type "1,2,3" par exemple
        std::stringstream saccess(access);
        std::string token;
        while(std::getline(saccess, token, ',')) {
            b.accessBuildings.push_back(std::stoi(token));
        }
        db[b.id] = b;
    }
    return db;
}

void logAction(const std::string &message, int rank) {
    std::ofstream logFile("log.txt", std::ios::app); // Ouvre le fichier en mode ajout
    if (!logFile.is_open()) {
        std::cerr << "[Processus " << rank << "] Erreur : Impossible d'ouvrir le fichier log.txt" << std::endl;
        return;
    }
    logFile << message << std::endl;
    logFile.close();
  
}

void sendLog(const std::string &message, int rank) {
    int messageLen = message.size();
    MPI_Send(&messageLen, 1, MPI_INT, 0, 60, MPI_COMM_WORLD); // Envoi de la taille
    MPI_Send(message.c_str(), messageLen, MPI_CHAR, 0, 60, MPI_COMM_WORLD); // Envoi du message
}

void buildingProcess(int rank) {
    std::set<int> badgesInside; // Track badges currently inside the building
    MPI_Status status;

    while (true) {
        int data[2]; // [badgeID, action]
        MPI_Recv(data, 2, MPI_INT, MPI_ANY_SOURCE, 20, MPI_COMM_WORLD, &status);
        int badgeID = data[0];
        int action = data[1];

        int response = 0; // Default response is "access denied"

        if (action == 1) { // Enter
            if (badgesInside.find(badgeID) == badgesInside.end()) {
                badgesInside.insert(badgeID); // Add badge to the set
                response = 1; // Access granted
            }
        } else if (action == 2) { // Exit
            if (badgesInside.find(badgeID) != badgesInside.end()) {
                badgesInside.erase(badgeID); // Remove badge from the set
                response = 1; // Access granted
            }
        }

        // Send the response back
        MPI_Send(&response, 1, MPI_INT, status.MPI_SOURCE, 30, MPI_COMM_WORLD);
    }
}


bool hasAccess(const BadgeInfo &b, int building) {
    return std::find(b.accessBuildings.begin(), b.accessBuildings.end(), building) != b.accessBuildings.end();
}

int main(int argc, char** argv) {
    MPI_Init(&argc,&argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD,&rank);
    MPI_Comm_size(MPI_COMM_WORLD,&size);

    int B = 3; // nombre de bâtiments

    if(size < B+2) {
            std::cerr << "Il faut au moins " << B+2 << " processus !" << std::endl;
            MPI_Finalize();
            return 1;
    }

    // Salle de commande
    if(rank == 0) {
        std::ofstream logFile("log.txt", std::ios::out); // Crée un fichier vide au début
        logFile.close();
        std::cout << "Log system initialisé, fichier log.txt créé." << std::endl;

        // Gestion des logs (thread séparé pour écouter en continu)
        std::thread logListener([&]() {
            bool running = true;
            while (running) {
                MPI_Status status;
                int flag;
                MPI_Iprobe(MPI_ANY_SOURCE, 60, MPI_COMM_WORLD, &flag, &status); // Vérifie si un log arrive
                if (flag) {
                    int source = status.MPI_SOURCE;
                    int messageLen;
                    MPI_Recv(&messageLen, 1, MPI_INT, source, 60, MPI_COMM_WORLD, &status); // Taille du message
                    std::vector<char> buffer(messageLen + 1, '\0');
                    MPI_Recv(buffer.data(), messageLen, MPI_CHAR, source, 60, MPI_COMM_WORLD, &status); // Message
                    std::string logMessage = buffer.data();
                    logAction(logMessage, rank); // Écriture synchronisée dans le fichier
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Pause pour éviter une boucle occupée
            }
        });

        // Chargement de la base de données
        std::map<int,BadgeInfo> db = loadBadgesFromFile("badges.txt");
        
        // Distribution initiale de la base aux bâtiments
        // On envoie la taille, puis chaque entrée
        for(int b=1; b<=B; b++) {
            int dbSize = (int)db.size();
            MPI_Send(&dbSize,1,MPI_INT,b,10,MPI_COMM_WORLD);
            for(auto &kv : db) {
                // Envoi de l'id
                MPI_Send(&kv.second.id,1,MPI_INT,b,10,MPI_COMM_WORLD);
                // Envoi du nom
                int nameLen = (int)kv.second.name.size();
                MPI_Send(&nameLen,1,MPI_INT,b,10,MPI_COMM_WORLD);
                MPI_Send(kv.second.name.c_str(),nameLen,MPI_CHAR,b,10,MPI_COMM_WORLD);
                // Envoi des accès
                int acSize = (int)kv.second.accessBuildings.size();
                MPI_Send(&acSize,1,MPI_INT,b,10,MPI_COMM_WORLD);
                MPI_Send(kv.second.accessBuildings.data(),acSize,MPI_INT,b,10,MPI_COMM_WORLD);
            }
        }

        // Simulation de quelques actions depuis la salle de commande
        // Par exemple, après quelques secondes, déclencher un incendie dans le bâtiment 2
        std::this_thread::sleep_for(std::chrono::seconds(5));
        int fireBuilding = 2;
        // On envoie un message d'incendie au bâtiment 2
        MPI_Send(&fireBuilding,1,MPI_INT,fireBuilding,40,MPI_COMM_WORLD);

        // Attendre la liste des présents en cas d'incendie
        // Le bâtiment 2 va répondre
        MPI_Status status;
        int nbPresent;
        MPI_Recv(&nbPresent,1,MPI_INT,fireBuilding,50,MPI_COMM_WORLD,&status);
        std::cout << "Incendie dans le bâtiment " << fireBuilding 
                  << ". Nombre de personnes présentes : " << nbPresent << std::endl;
        for(int i=0; i<nbPresent; i++) {
            int nameLen;
            MPI_Recv(&nameLen,1,MPI_INT,fireBuilding,50,MPI_COMM_WORLD,&status);
            std::vector<char> nameBuf(nameLen+1,'\0');
            MPI_Recv(nameBuf.data(),nameLen,MPI_CHAR,fireBuilding,50,MPI_COMM_WORLD,&status);
            std::cout << " - " << nameBuf.data() << std::endl;
        }

        logListener.join();
    }

    else if(rank >= 1 && rank <= B) {
        // Processus bâtiment
        // Réception de la base de données initiale
        MPI_Status status;
        int dbSize;
        MPI_Recv(&dbSize,1,MPI_INT,0,10,MPI_COMM_WORLD,&status);
        std::map<int,BadgeInfo> localDB;
        for(int i=0; i<dbSize; i++){
            int id;
            MPI_Recv(&id,1,MPI_INT,0,10,MPI_COMM_WORLD,&status);
            int nameLen;
            MPI_Recv(&nameLen,1,MPI_INT,0,10,MPI_COMM_WORLD,&status);
            std::vector<char> nameBuf(nameLen+1,'\0');
            MPI_Recv(nameBuf.data(),nameLen,MPI_CHAR,0,10,MPI_COMM_WORLD,&status);
            int acSize;
            MPI_Recv(&acSize,1,MPI_INT,0,10,MPI_COMM_WORLD,&status);
            std::vector<int> accessVec(acSize);
            MPI_Recv(accessVec.data(),acSize,MPI_INT,0,10,MPI_COMM_WORLD,&status);

            BadgeInfo b;
            b.id=id;
            b.name = nameBuf.data();
            b.accessBuildings = accessVec;
            localDB[id] = b;
        }

        std::vector<std::string> presentPersons; 
        bool fireAlarm = false;

        // On lance un thread pour écouter les demandes d'accès et d'autres messages
        bool running = true;
        
        // Pour la simulation, on utilise MPI_Iprobe dans une boucle
        while(running) {
            int flag;
            MPI_Status st;
            MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &st);
            if(flag) {
                if(st.MPI_TAG == 20) {
                    // Demande d'accès
                    // Message du type : [badgeID, typeAction(1=entrée,2=sortie)]
                    int data[2];
                    MPI_Recv(&data,2,MPI_INT,st.MPI_SOURCE,20,MPI_COMM_WORLD,&st);
                    int badgeID = data[0];
                    int action = data[1]; 
                    // action: 1=entrée, 2=sortie
                    bool allowed = fireAlarm || (localDB.find(badgeID) != localDB.end() && hasAccess(localDB[badgeID], rank));
                    if(action == 1) {
                        // Demande d'entrée
                        if(allowed) {
                            sendLog("Badge " + std::to_string(badgeID) + " est entré dans le bâtiment " + std::to_string(rank), rank);
                            // Simuler voyant vert + entrée
                            presentPersons.push_back(localDB[badgeID].name);
                            // Envoyer réponse OK
                            int response = 1; // 1=OK
                            MPI_Send(&response,1,MPI_INT,st.MPI_SOURCE,30,MPI_COMM_WORLD);
                        } else {
                            int response = 0; // 0=refus
                            MPI_Send(&response,1,MPI_INT,st.MPI_SOURCE,30,MPI_COMM_WORLD);
                            sendLog("Badge " + std::to_string(badgeID) + " refusé dans " + std::to_string(rank), rank);
                        }
                    } else {
                        // Demande de sortie
                        // On vérifie si la personne est dedans
                        auto it = std::find(presentPersons.begin(), presentPersons.end(), localDB[badgeID].name);
                        if(it != presentPersons.end()) {
                            presentPersons.erase(it);
                            int response = 1; // sortie OK
                            MPI_Send(&response,1,MPI_INT,st.MPI_SOURCE,30,MPI_COMM_WORLD);
                            sendLog("Badge " + std::to_string(badgeID) + " est sorti du bâtiment " + std::to_string(rank), rank);
                        } else {
                            int response = 0; // pas présent?
                            MPI_Send(&response,1,MPI_INT,st.MPI_SOURCE,30,MPI_COMM_WORLD);
                            sendLog("Badge " + std::to_string(badgeID) + " n'est pas présent dans bâtiment " + std::to_string(rank), rank);
                        }
                    }
                } else if(st.MPI_TAG == 40) {
                    // Incendie
                    int buildingOnFire;
                    MPI_Recv(&buildingOnFire,1,MPI_INT,0,40,MPI_COMM_WORLD,&st);
                    sendLog("Incendie dans le bâtiment " + std::to_string(rank) + ", alarme activée", rank);
                    if(buildingOnFire == rank) {
                        fireAlarm = true;
                        // Envoyer la liste des présents à la salle de commande
                        int nbPresent = (int)presentPersons.size();
                        MPI_Send(&nbPresent,1,MPI_INT,0,50,MPI_COMM_WORLD);
                        for(auto &p : presentPersons) {
                            int len = (int)p.size();
                            MPI_Send(&len,1,MPI_INT,0,50,MPI_COMM_WORLD);
                            MPI_Send(p.c_str(),len,MPI_CHAR,0,50,MPI_COMM_WORLD);
                        }
                    }
                } else {
                    // Autres messages éventuels
                    // Pour l'exemple, on arrête après un certain temps
                }
            }
            // Petite pause
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    else if(rank == 4) {
        // Simulateurs
        // Ils envoient périodiquement des demandes d'accès
        // Par exemple, le simulateur rank=B+1 va tenter d'entrer dans le bâtiment 2 avec le badge 101
        int targetBuilding = 2;
        int badgeID = 101;
        int action = 1; // entrée
        int data[2]={badgeID,action};
        MPI_Send(data,2,MPI_INT,targetBuilding,20,MPI_COMM_WORLD);
        // Attendre la réponse
        MPI_Status st;
        int response;
        MPI_Recv(&response,1,MPI_INT,targetBuilding,30,MPI_COMM_WORLD,&st);
        if(response == 1)
            std::cout << "[Simulateur " << rank << "] Entrée autorisée pour badge " << badgeID << " dans bâtiment " << targetBuilding << std::endl;
        else
            std::cout << "[Simulateur " << rank << "] Entrée refusée pour badge " << badgeID << std::endl;
        
    }

    else if(rank == 5) {
        // Simulateurs
        // Ils envoient périodiquement des demandes d'accès
        // Par exemple, le simulateur rank=B+1 va tenter d'entrer dans le bâtiment 2 avec le badge 101
        int targetBuilding = 1;
        int badgeID = 102;
        int action = 1; // entrée
        int data[2]={badgeID,action};
        MPI_Send(data,2,MPI_INT,targetBuilding,20,MPI_COMM_WORLD);
        // Attendre la réponse
        MPI_Status st;
        int response;
        MPI_Recv(&response,1,MPI_INT,targetBuilding,30,MPI_COMM_WORLD,&st);
        if(response == 1)
            std::cout << "[Simulateur " << rank << "] Entrée autorisée pour badge " << badgeID << " dans bâtiment " << targetBuilding << std::endl;
        else
            std::cout << "[Simulateur " << rank << "] Entrée refusée pour badge " << badgeID << std::endl;
        
    }
    
    else if(rank == 6) {
        // Simulateurs
        // Ils envoient périodiquement des demandes d'accès
        // Par exemple, le simulateur rank=B+1 va tenter d'entrer dans le bâtiment 2 avec le badge 101
        int targetBuilding = 2;
        int badgeID = 200;
        int action = 1; // entrée
        int data[2]={badgeID,action};
        MPI_Send(data,2,MPI_INT,targetBuilding,20,MPI_COMM_WORLD);
        // Attendre la réponse
        MPI_Status st;
        int response;
        MPI_Recv(&response,1,MPI_INT,targetBuilding,30,MPI_COMM_WORLD,&st);
        if(response == 1)
            std::cout << "[Simulateur " << rank << "] Entrée autorisée pour badge " << badgeID << " dans bâtiment " << targetBuilding << std::endl;
        else
            std::cout << "[Simulateur " << rank << "] Entrée refusée pour badge " << badgeID << std::endl;
        
    }

    MPI_Finalize();
    return 0;
}