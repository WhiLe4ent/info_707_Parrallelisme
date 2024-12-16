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

std::mutex logMutex;

void logAction(const std::string &message, int rank) {
    std::lock_guard<std::mutex> lock(logMutex);
    std::ofstream logFile("log.txt", std::ios::app); // Ouvre le fichier en mode ajout
    if (!logFile.is_open()) {
        std::cerr << "[Processus " << rank << "] Erreur : Impossible d'ouvrir le fichier log.txt" << std::endl;
        return;
    }
    logFile << message << std::endl;
    logFile.close();
}

void sendLog(const std::string &message, int rank) {
    int messageLen = (int)message.size();
    MPI_Send(&messageLen, 1, MPI_INT, 0, 60, MPI_COMM_WORLD); // Envoi de la taille
    MPI_Send(message.c_str(), messageLen, MPI_CHAR, 0, 60, MPI_COMM_WORLD); // Envoi du message
}

bool hasAccess(const BadgeInfo &b, int building) {
    return std::find(b.accessBuildings.begin(), b.accessBuildings.end(), building) != b.accessBuildings.end();
}

void displayLogs() {
    std::lock_guard<std::mutex> lock(logMutex);
    std::ifstream logFile("log.txt");
    if (!logFile.is_open()) {
        std::cerr << "Impossible d'ouvrir le fichier log.txt" << std::endl;
        return;
    }
    std::string line;
    std::cout << "\n===== Journaux =====\n";
    while (std::getline(logFile, line)) {
        std::cout << line << std::endl;
    }
    std::cout << "====================\n";
    logFile.close();
}

void simulatorProcess(int rank) {
    // Le simulateur attend des instructions du processus 0 (salle de commande)
    // Il reçoit un message du type : data[3] = {badgeID, targetBuilding, action}
    // Puis envoie une demande d'accès au bâtiment spécifié et affiche la réponse.
    MPI_Status status;
    while (true) {
        int data[3]; // [badgeID, targetBuilding, action (1=entrée,2=sortie)]
        MPI_Recv(data, 3, MPI_INT, 0, 70, MPI_COMM_WORLD, &status);
        int badgeID = data[0];
        int targetBuilding = data[1];
        int action = data[2];

        // Envoyer la demande d'accès au bâtiment
        int accessData[2] = {badgeID, action};
        MPI_Send(accessData, 2, MPI_INT, targetBuilding, 20, MPI_COMM_WORLD);

        // Attendre la réponse du bâtiment
        int response;
        MPI_Recv(&response, 1, MPI_INT, targetBuilding, 30, MPI_COMM_WORLD, &status);
        if (response == 1) {
            std::cout << "[Simulateur " << rank << "] Accès autorisé pour le badge " << badgeID
                      << " dans le bâtiment " << targetBuilding << ".\n";
        } else {
            std::cout << "[Simulateur " << rank << "] Accès refusé pour le badge " << badgeID
                      << " dans le bâtiment " << targetBuilding << ".\n";
        }
    }
}

int main(int argc, char** argv) {
    MPI_Init(&argc,&argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD,&rank);
    MPI_Comm_size(MPI_COMM_WORLD,&size);

    int B = 3; // nombre de bâtiments
    int NB_SIMUL = 2; // nombre de simulateurs, par exemple
    // On aura donc les processus : 0 = salle de commande, 1..B = bâtiments, B+1..B+NB_SIMUL = simulateurs

    if(size < B + NB_SIMUL + 1) {
        std::cerr << "Il faut au moins " << B + NB_SIMUL + 1 << " processus !" << std::endl;
        MPI_Finalize();
        return 1;
    }

    if(rank == 0) {
        // Salle de commande
        // Initialisation du log
        {
            std::ofstream logFile("log.txt", std::ios::out);
            logFile.close();
        }
        std::cout << "Log system initialisé, fichier log.txt créé." << std::endl << std::flush;

        // Gestion des logs en parallèle : on lance un thread qui écoute en continu les logs
        std::thread logListener([&]() {
            bool running = true;
            MPI_Status status;
            while (running) {
                int flag;
                MPI_Iprobe(MPI_ANY_SOURCE, 60, MPI_COMM_WORLD, &flag, &status);
                if (flag) {
                    int source = status.MPI_SOURCE;
                    int messageLen;
                    MPI_Recv(&messageLen, 1, MPI_INT, source, 60, MPI_COMM_WORLD, &status);
                    std::vector<char> buffer(messageLen + 1, '\0');
                    MPI_Recv(buffer.data(), messageLen, MPI_CHAR, source, 60, MPI_COMM_WORLD, &status);
                    std::string logMessage = buffer.data();
                    logAction(logMessage, 0);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });

        // Chargement de la base de données
        std::map<int,BadgeInfo> db = loadBadgesFromFile("badges.txt");
        // Distribution de la DB aux bâtiments
        for(int b=1; b<=B; b++) {
            int dbSize = (int)db.size();
            MPI_Send(&dbSize,1,MPI_INT,b,10,MPI_COMM_WORLD);
            for(auto &kv : db) {
                MPI_Send(&kv.second.id,1,MPI_INT,b,10,MPI_COMM_WORLD);
                int nameLen = (int)kv.second.name.size();
                MPI_Send(&nameLen,1,MPI_INT,b,10,MPI_COMM_WORLD);
                MPI_Send(kv.second.name.c_str(),nameLen,MPI_CHAR,b,10,MPI_COMM_WORLD);
                int acSize = (int)kv.second.accessBuildings.size();
                MPI_Send(&acSize,1,MPI_INT,b,10,MPI_COMM_WORLD);
                MPI_Send(kv.second.accessBuildings.data(),acSize,MPI_INT,b,10,MPI_COMM_WORLD);
            }
        }

        // Menu interactif
        while (true) {
            std::cout << "\n===== Menu Interactif =====\n" << std::flush;
            std::cout << "1. Déclencher un incendie\n" << std::flush;
            std::cout << "2. Simuler une demande d'accès\n" << std::flush;
            std::cout << "3. Consulter les journaux\n" << std::flush;
            std::cout << "4. Quitter\n" << std::flush;
            std::cout << "Choix : " << std::flush;

            int choix;
            std::cin >> choix;

            if (choix == 1) {
                std::cout << "Entrez le numéro du bâtiment pour déclencher un incendie : " << std::flush;
                int fireBuilding;
                std::cin >> fireBuilding;
                if(fireBuilding < 1 || fireBuilding > B) {
                    std::cout << "Numéro de bâtiment invalide.\n" << std::flush;
                    continue;
                }
                MPI_Send(&fireBuilding, 1, MPI_INT, fireBuilding, 40, MPI_COMM_WORLD);
                std::cout << "Incendie déclenché dans le bâtiment " << fireBuilding << ". Attente des données...\n" << std::flush;

                // Le bâtiment en feu répond en envoyant la liste des présents
                MPI_Status st;
                int nbPresent;
                MPI_Recv(&nbPresent,1,MPI_INT,fireBuilding,50,MPI_COMM_WORLD,&st);
                std::cout << "Nombre de personnes présentes dans le bâtiment " << fireBuilding
                          << " : " << nbPresent << std::endl << std::flush;
                for(int i=0; i<nbPresent; i++) {
                    int nameLen;
                    MPI_Recv(&nameLen,1,MPI_INT,fireBuilding,50,MPI_COMM_WORLD,&st);
                    std::vector<char> nameBuf(nameLen+1,'\0');
                    MPI_Recv(nameBuf.data(),nameLen,MPI_CHAR,fireBuilding,50,MPI_COMM_WORLD,&st);
                    std::cout << " - " << nameBuf.data() << std::endl << std::flush;
                }

            } else if (choix == 2) {
                std::cout << "Entrez l'ID du badge : " << std::flush;
                int badgeID;
                std::cin >> badgeID;
                std::cout << "Entrez le numéro du bâtiment : " << std::flush;
                int targetBuilding;
                std::cin >> targetBuilding;
                if(targetBuilding < 1 || targetBuilding > B) {
                    std::cout << "Numéro de bâtiment invalide.\n" << std::flush;
                    continue;
                }
                std::cout << "Action (1 = entrée, 2 = sortie) : " << std::flush;
                int action;
                std::cin >> action;
                if(action != 1 && action != 2) {
                    std::cout << "Action invalide.\n" << std::flush;
                    continue;
                }

                int data[3] = {badgeID, targetBuilding, action};
                int simulatorRank = B + 1; // On envoie toujours au premier simulateur
                MPI_Send(data, 3, MPI_INT, simulatorRank, 70, MPI_COMM_WORLD);
                std::cout << "Demande envoyée au simulateur " << simulatorRank << ".\n" << std::flush;

            } else if (choix == 3) {
                displayLogs();
            } else if (choix == 4) {
                std::cout << "Fermeture du système.\n" << std::flush;
                // Fin du programme
                break;
            } else {
                std::cout << "Choix invalide, veuillez réessayer.\n" << std::flush;
            }
        }

        // On détache le thread de logs pour laisser MPI_Finalize nettoyer
        logListener.detach();
    }

    else if(rank >= 1 && rank <= B) {
        // Processus bâtiment
        MPI_Status status;
        int dbSize;
        MPI_Recv(&dbSize,1,MPI_INT,0,10,MPI_COMM_WORLD,&status);
        std::map<int,BadgeInfo> localDB;
        for(int i=0; i<dbSize; i++) {
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

        std::vector<int> badgesInside;
        bool fireAlarm = false;

        // On utilise MPI_Iprobe pour gérer plusieurs types de messages
        while (true) {
            MPI_Status st;
            int flag;
            MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &st);
            if(flag) {
                if(st.MPI_TAG == 20) {
                    // Demande d'accès (entrée/sortie)
                    int data[2];
                    MPI_Recv(data,2,MPI_INT,st.MPI_SOURCE,20,MPI_COMM_WORLD,&st);
                    int badgeID = data[0];
                    int action = data[1]; // 1=entrée, 2=sortie

                    int response = 0;
                    // Vérification des droits d'accès si pas d'alarme
                    bool allowed = fireAlarm ||
                                   (localDB.find(badgeID) != localDB.end() &&
                                    hasAccess(localDB[badgeID], rank));
                    if(action == 1) {
                        // Entrée
                        if(allowed && std::find(badgesInside.begin(), badgesInside.end(), badgeID) == badgesInside.end()) {
                            badgesInside.push_back(badgeID);
                            response = 1;
                            sendLog("Badge " + std::to_string(badgeID) + " est entré dans le bâtiment " + std::to_string(rank), rank);
                        } else {
                            sendLog("Badge " + std::to_string(badgeID) + " refusé à l'entrée du bâtiment " + std::to_string(rank), rank);
                        }
                    } else {
                        // Sortie
                        auto it = std::find(badgesInside.begin(), badgesInside.end(), badgeID);
                        if(it != badgesInside.end()) {
                            badgesInside.erase(it);
                            response = 1;
                            sendLog("Badge " + std::to_string(badgeID) + " est sorti du bâtiment " + std::to_string(rank), rank);
                        } else {
                            sendLog("Badge " + std::to_string(badgeID) + " a demandé à sortir du bâtiment " + std::to_string(rank) + " mais n'est pas à l'intérieur", rank);
                        }
                    }
                    MPI_Send(&response,1,MPI_INT,st.MPI_SOURCE,30,MPI_COMM_WORLD);
                } else if(st.MPI_TAG == 40) {
                    // Incendie
                    int buildingOnFire;
                    MPI_Recv(&buildingOnFire,1,MPI_INT,0,40,MPI_COMM_WORLD,&st);
                    sendLog("Incendie dans le bâtiment " + std::to_string(buildingOnFire) + ", alarme activée", rank);

                    if(buildingOnFire == rank) {
                        fireAlarm = true;
                        
                        // Log supplémentaire indiquant que les portes sont ouvertes pour évacuation
                        if(!badgesInside.empty()) {
                            sendLog("Le bâtiment " + std::to_string(rank) + " est en feu, les portes sont ouvertes pour évacuation d'urgence.", rank);
                        } else {
                            sendLog("Le bâtiment " + std::to_string(rank) + " est en feu, portes ouvertes (aucune personne à l'intérieur).", rank);
                        }

                        // Envoyer la liste des présents à la salle de commande
                        int nbPresent = (int)badgesInside.size();
                        MPI_Send(&nbPresent,1,MPI_INT,0,50,MPI_COMM_WORLD);
                        for(auto bid : badgesInside) {
                            std::string personName = localDB[bid].name;
                            int len = (int)personName.size();
                            MPI_Send(&len,1,MPI_INT,0,50,MPI_COMM_WORLD);
                            MPI_Send(personName.c_str(),len,MPI_CHAR,0,50,MPI_COMM_WORLD);
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    else if(rank > B) {
        // Processus simulateur
        simulatorProcess(rank);
    }

    MPI_Finalize();
    return 0;
}