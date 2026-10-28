// Le modèle de langue, dans un PROCESSUS NATIF.
//
// Pourquoi ce fichier existe — demande répétée de l'utilisateur, « je t'ai dit
// de ne pas exécuter ça dans la JVM ». Le modèle a d'abord vécu dans le
// processus du clavier : Android tuait le processus pour récupérer ses 2,9 Go
// et le clavier disparaissait sous les doigts. Il est ensuite passé dans un
// service Android à part (`:llm`) : le clavier a survécu, mais le processus du
// modèle, lui, mourait toujours — mesuré le 2026-10-25, `importance=400` puis
// `SIGKILL` hors de tout registre d'ActivityManager. Un processus
// d'application reste un processus d'application : le gestionnaire le classe,
// le rétrograde en « vide », et le prend.
//
// Ici, ce n'est plus une application Android. C'est un exécutable lancé par le
// clavier, qu'ActivityManager ne connaît pas, ne classe pas et ne rétrograde
// donc jamais : il garde l'`oom_score_adj` hérité au lancement. Pas d'ART, pas
// de classe `Application`, pas de tas Dalvik — rien que llama.cpp et deux
// tuyaux.
//
// Il est nommé `libllm_serveur.so` : Android n'autorise l'exécution que depuis
// le répertoire des bibliothèques natives de l'application, et n'y extrait que
// les fichiers `lib*.so`. C'est un exécutable malgré le nom.
//
// Dialogue sur l'entrée et la sortie standard, en longueurs préfixées plutôt
// qu'en échappements : un jeton peut contenir n'importe quel octet, retours à
// la ligne et tabulations comprises.
//
//   CHARGER \t chemin \t n_ctx \t fils \t couches_gpu \t mmap \t mlock \t eclair \t lot
//           \t budget_mio      (couches_gpu = -1 : le moteur ajuste au budget)
//     → PRET \t gpu            ou  ERREUR \t code
//   GENERER \t jetons_max \t temp‰ \t top_k \t top_p‰ \t min_p‰ \t
//            penalite‰ \t graine \t octets_invite \t nb_arrets
//     puis l'invite (octets bruts), puis chaque séquence d'arrêt (longueur
//     sur sa ligne, puis les octets)
//     → JETON \t n \n <n octets>   (autant de fois que de jetons)
//     → FINI  \t n \n <n octets>
//     → ERREUR \t code
//   INTERROMPRE   (lu pendant la génération, par le fil lecteur)
//   LIBERER       → LIBERE
//   QUITTER       → sortie
//
// Les flottants passent en millièmes entiers : pas de virgule décimale à
// interpréter d'un côté comme de l'autre selon la langue du système.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <sys/prctl.h>
#include <sys/resource.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>

#include "llama.h"
#include "rn-llama.h"
#include "rn-completion.h"

namespace {

rnllama::llama_rn_context *moteur = nullptr;

/// Posé par le fil lecteur, lu par la boucle de génération.
std::atomic<bool> interruption{false};

std::mutex sortie_verrou;

void ecrire_ligne(const std::string &ligne) {
    std::lock_guard<std::mutex> v(sortie_verrou);
    fwrite(ligne.data(), 1, ligne.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

/// En-tête puis octets bruts : aucun échappement, donc aucun jeton ne peut
/// casser le protocole.
void ecrire_bloc(const char *mot, const std::string &charge) {
    std::lock_guard<std::mutex> v(sortie_verrou);
    fprintf(stdout, "%s\t%zu\n", mot, charge.size());
    fwrite(charge.data(), 1, charge.size(), stdout);
    fflush(stdout);
}

/// Les traces de llama.cpp vont sur la sortie d'erreur, que le clavier relaie
/// dans son journal : elles ne doivent surtout pas se mélanger au protocole.
void trace(lm_ggml_log_level level, const char *texte, void *) {
    if (level == LM_GGML_LOG_LEVEL_DEBUG) return;
    fputs(texte, stderr);
    fflush(stderr);
}

std::vector<std::string> decouper(const std::string &ligne, char sep) {
    std::vector<std::string> morceaux;
    size_t debut = 0;
    while (true) {
        size_t pos = ligne.find(sep, debut);
        if (pos == std::string::npos) {
            morceaux.push_back(ligne.substr(debut));
            break;
        }
        morceaux.push_back(ligne.substr(debut, pos - debut));
        debut = pos + 1;
    }
    return morceaux;
}

bool lire_ligne(std::string &ligne) {
    ligne.clear();
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (c == '\n') return true;
        ligne.push_back(static_cast<char>(c));
    }
    // Fin de l'entrée : le clavier a fermé le tuyau, ou il est mort. Dans les
    // deux cas il n'y a plus personne à servir, et garder des gigaoctets en
    // mémoire serait le pire des services à rendre au téléphone.
    return false;
}

bool lire_octets(size_t combien, std::string &dans) {
    dans.resize(combien);
    size_t lus = 0;
    while (lus < combien) {
        size_t n = fread(&dans[lus], 1, combien - lus, stdin);
        if (n == 0) return false;
        lus += n;
    }
    return true;
}

int entier(const std::string &s, int defaut) {
    try {
        return std::stoi(s);
    } catch (...) {
        return defaut;
    }
}

float millieme(const std::string &s, float defaut) {
    try {
        return static_cast<float>(std::stoi(s)) / 1000.0f;
    } catch (...) {
        return defaut;
    }
}

void charger(const std::vector<std::string> &champs) {
    if (champs.size() < 7) {
        ecrire_ligne("ERREUR\tcommande_incomplete");
        return;
    }
    if (moteur != nullptr) {
        delete moteur;
        moteur = nullptr;
    }

    common_params params;
    params.model.path = champs[1];
    params.n_ctx = entier(champs[2], 2048);
    params.cpuparams.n_threads = entier(champs[3], 4);
    const int couches = entier(champs[4], 0);
    const bool mmap = entier(champs[5], 1) != 0;
    const bool mlock = entier(champs[6], 0) != 0;
    // Attention éclair : demandée par le clavier, pas décidée ici.
    //
    // Mesure du 2026-10-25 : avec le GPU, le pilote Adreno compile ses noyaux
    // d'attention éclair au premier chargement, et c'est LÀ que le téléphone
    // s'effondre — le processus est tué pendant « compiling fa f32_f16
    // split », pendant que le système sacrifie six applications. Les poids, eux,
    // ne pèsent que 921 Mio sur le GPU. Par défaut (champ absent) elle est
    // donc DÉSACTIVÉE : le clavier la rallumera quand les noyaux seront en
    // cache sur le disque.
    const int eclair = champs.size() > 7 ? entier(champs[7], 0) : 0;
    // Taille de lot, demandée par le clavier.
    //
    // Mesure du 2026-10-25 : avec le GPU, le tampon de calcul OpenCL coûte
    // 300 Mio pour un lot de 512, sur un plafond mesuré entre 480 et 845 Mio
    // avant que `lowmemorykiller` ne prenne le processus. C'est autant de
    // couches qu'on ne peut pas déporter, pour un lot que les invites du
    // clavier n'atteignent jamais.
    const int n_lot = champs.size() > 8 ? entier(champs[8], 512) : 512;
    // Budget mémoire du GPU, en mébioctets, et c'est llama.cpp qui en déduit
    // le nombre de couches (`fit_params_target`).
    //
    // Mesure du 2026-10-25 : en imposant 999 couches, on empêchait le moteur
    // de faire ce travail — il déportait tout, le pilote réclamait 921 Mio de
    // poids plus 300 de calcul, et `lowmemorykiller` prenait le processus. Le
    // moteur, lui, sait compter les poids, le cache d'attention et les tampons
    // de calcul ensemble. Autant le laisser faire, avec le vrai budget plutôt
    // que les 4 630 Mio que le pilote prétend libres.
    const int budget_mio = champs.size() > 9 ? entier(champs[9], 0) : 0;
    params.n_batch = n_lot > 0 ? n_lot : 512;
    // Le lot physique suit le lot logique : le laisser plus grand ferait
    // réserver au backend des tampons pour des lots qui n'arriveront jamais.
    params.n_ubatch = params.n_batch;
    if (mmap && mlock) {
        params.load_mode = LLAMA_LOAD_MODE_MMAP_MLOCK;
    } else if (mmap) {
        params.load_mode = LLAMA_LOAD_MODE_MMAP;
    } else if (mlock) {
        params.load_mode = LLAMA_LOAD_MODE_MLOCK;
    } else {
        params.load_mode = LLAMA_LOAD_MODE_NONE;
    }
    // L'échauffement parcourt le modèle entier au chargement et fait remonter
    // chaque page en mémoire : exactement ce qu'on ne veut pas sur un
    // téléphone sous pression. Il se fera à la première vraie génération.
    params.warmup = false;
    params.embedding = false;
    params.n_parallel = 1;
    params.flash_attn_type = eclair > 0 ? LLAMA_FLASH_ATTN_TYPE_ENABLED
                                        : LLAMA_FLASH_ATTN_TYPE_DISABLED;

    if (couches < 0 && budget_mio > 0) {
        // Couches laissées « non fixées » : le moteur les ajuste au budget.
        params.fit_params = true;
        const size_t budget = (size_t) budget_mio * 1024ull * 1024ull;
        params.fit_params_target.assign(params.fit_params_target.size(), budget);
    } else {
        // Valeur imposée par le clavier (0 = processeur seul) : on coupe
        // l'ajustement, sinon le moteur pourrait la remonter.
        params.n_gpu_layers = couches < 0 ? 0 : couches;
        params.fit_params = false;
    }

    // llama.cpp monte ses files de fils dans le constructeur de
    // `common_init_result`, sans garde-fou : la file de LOT hérite du -1 des
    // paramètres par défaut, réclame 544 octets × -1 de travailleurs et écrit
    // dans le pointeur nul que l'allocateur renvoie (plantage mesuré le
    // 2026-10-24, `SIGSEGV` dans `__memset_aarch64`). L'exécutable amont
    // normalise ces champs à l'analyse de ses arguments ; nous n'y passons
    // pas, donc on le fait ici.
    postprocess_cpu_params(params.cpuparams);
    postprocess_cpu_params(params.cpuparams_batch, &params.cpuparams);

    moteur = new rnllama::llama_rn_context();
    const bool ok = moteur->loadModel(params);
    if (!ok || moteur->ctx == nullptr) {
        delete moteur;
        moteur = nullptr;
        ecrire_ligne("ERREUR\tchargement_impossible");
        return;
    }
    // « Sur le GPU » dit ce qu'on a DEMANDÉ : l'ajustement travaille sur une
    // copie des paramètres, et le compte exact des couches déportées n'est
    // lisible que dans les traces du moteur (« offloaded N/M layers to GPU »),
    // que le clavier relaie dans son journal.
    const bool sur_gpu = couches != 0;
    ecrire_ligne(std::string("PRET\t") + (sur_gpu ? "1" : "0"));
}

void generer(const std::vector<std::string> &champs) {
    if (champs.size() < 10) {
        ecrire_ligne("ERREUR\tcommande_incomplete");
        return;
    }
    const size_t octets_invite = static_cast<size_t>(entier(champs[8], 0));
    const int nb_arrets = entier(champs[9], 0);

    std::string invite;
    if (!lire_octets(octets_invite, invite)) {
        ecrire_ligne("ERREUR\tinvite_tronquee");
        return;
    }
    std::vector<std::string> arrets;
    for (int i = 0; i < nb_arrets; i++) {
        std::string ligne;
        if (!lire_ligne(ligne)) return;
        std::string arret;
        if (!lire_octets(static_cast<size_t>(entier(ligne, 0)), arret)) return;
        arrets.push_back(arret);
    }

    if (moteur == nullptr || moteur->completion == nullptr) {
        ecrire_ligne("ERREUR\tnot_ready");
        return;
    }

    interruption = false;
    moteur->completion->rewind();
    moteur->params.prompt = invite;
    moteur->params.n_predict = entier(champs[1], 256);
    moteur->params.sampling.temp = millieme(champs[2], 0.7f);
    moteur->params.sampling.top_k = entier(champs[3], 40);
    moteur->params.sampling.top_p = millieme(champs[4], 0.95f);
    moteur->params.sampling.min_p = millieme(champs[5], 0.05f);
    // La pénalité de répétition corrige le charabia répétitif ; elle était
    // transmise par le clavier mais le pont JNI ne la posait jamais.
    moteur->params.sampling.penalty_repeat = millieme(champs[6], 1.1f);
    const int graine = entier(champs[7], -1);
    moteur->params.sampling.seed = graine < 0 ? static_cast<uint32_t>(time(nullptr))
                                              : static_cast<uint32_t>(graine);
    // Les séquences d'arrêt viennent de `llm_prompt.rs` : ce sont les
    // marqueurs de fin de tour du patron du modèle, que l'EOS du GGUF ne
    // couvre pas. Le pont JNI les recevait aussi sans jamais les poser, et le
    // modèle écrivait la suite de la conversation après sa réponse.
    moteur->params.antiprompt = arrets;

    moteur->completion->initSampling();

    std::string tout;
    try {
        moteur->completion->loadPrompt({});
        moteur->completion->beginCompletion();

        size_t envoyes = 0;
        while (moteur->completion->has_next_token && !moteur->completion->is_interrupted) {
            if (interruption.load()) {
                moteur->completion->is_interrupted = true;
                break;
            }
            auto jeton = moteur->completion->doCompletion();
            if (jeton.tok == -1) break;
            if (moteur->completion->incomplete) continue;

            const size_t pos = std::min(envoyes, moteur->completion->generated_text.size());
            const std::string morceau = moteur->completion->generated_text.substr(pos);
            envoyes += morceau.size();
            if (!morceau.empty()) {
                tout += morceau;
                ecrire_bloc("JETON", morceau);
            }
        }
        moteur->completion->endCompletion();
    } catch (const std::exception &e) {
        fprintf(stderr, "génération interrompue par une exception : %s\n", e.what());
        fflush(stderr);
        ecrire_ligne("ERREUR\tgeneration_failed");
        return;
    } catch (...) {
        ecrire_ligne("ERREUR\tgeneration_failed");
        return;
    }
    ecrire_bloc("FINI", tout);
}

void liberer() {
    if (moteur != nullptr) {
        delete moteur;
        moteur = nullptr;
    }
    ecrire_ligne("LIBERE");
}

} // namespace

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;

    // Si le clavier meurt, ce processus meurt avec lui. Sans ça, un orphelin
    // garderait des gigaoctets jusqu'au redémarrage du téléphone — le
    // reproche exact qu'on cherche à ne plus mériter.
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    // Et si le clavier a disparu entre le `fork` et ici, personne ne nous
    // enverra ce signal : on le vérifie une fois.
    if (getppid() == 1) {
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);

    // Où ce processus a-t-il le droit de tourner ?
    //
    // Un fils hérite du groupe de contrôle de son parent, et ActivityManager
    // ne le déplacera jamais puisqu'il ne le connaît pas. Né dans le groupe
    // d'arrière-plan, il reste sur les petits cœurs — et un modèle de langue y
    // perd un facteur trois. Mesure du 2026-10-25 : Apertus 8B rendait
    // 3,53 jetons/s dans la JVM contre 1,25 ici, alors que le même binaire
    // lancé depuis un shell (groupe « top-app ») tenait 23,7 jetons/s sur un
    // autre modèle. On le relève, puis on tente d'y remédier.
    {
        char groupe[128] = "";
        FILE *f = fopen("/proc/self/cpuset", "r");
        if (f != nullptr) {
            if (fgets(groupe, sizeof(groupe), f) == nullptr) {
                groupe[0] = '\0';
            }
            fclose(f);
        }
        cpu_set_t avant;
        CPU_ZERO(&avant);
        int coeurs_avant = 0;
        if (sched_getaffinity(0, sizeof(avant), &avant) == 0) {
            coeurs_avant = CPU_COUNT(&avant);
        }
        const int politesse = getpriority(PRIO_PROCESS, 0);

        // Rendre tous les cœurs : si le groupe de contrôle l'interdit, l'appel
        // échoue sans rien casser — on le dira dans la trace.
        cpu_set_t tous;
        CPU_ZERO(&tous);
        const int n = (int) sysconf(_SC_NPROCESSORS_CONF);
        for (int i = 0; i < n && i < CPU_SETSIZE; i++) {
            CPU_SET(i, &tous);
        }
        const bool elargi = sched_setaffinity(0, sizeof(tous), &tous) == 0;

        cpu_set_t apres;
        CPU_ZERO(&apres);
        int coeurs_apres = 0;
        if (sched_getaffinity(0, sizeof(apres), &apres) == 0) {
            coeurs_apres = CPU_COUNT(&apres);
        }
        fprintf(stderr,
                "serveur: cpuset=%s coeurs=%d→%d (sur %d) politesse=%d elargissement=%s\n",
                groupe[0] != '\0' ? groupe : "?", coeurs_avant, coeurs_apres, n,
                politesse, elargi ? "accepté" : "refusé");
        fflush(stderr);
    }

    llama_log_set(trace, nullptr);
    llama_backend_init();

    std::string ligne;
    while (lire_ligne(ligne)) {
        if (ligne.empty()) continue;
        const std::vector<std::string> champs = decouper(ligne, '\t');
        const std::string &quoi = champs[0];
        if (quoi == "CHARGER") {
            charger(champs);
        } else if (quoi == "GENERER") {
            generer(champs);
        } else if (quoi == "INTERROMPRE") {
            // Arrivée pendant une génération, elle est vue par la boucle
            // ci-dessus ; arrivée entre deux, elle ne coûte rien.
            interruption = true;
            if (moteur != nullptr && moteur->completion != nullptr) {
                moteur->completion->is_interrupted = true;
            }
        } else if (quoi == "LIBERER") {
            liberer();
        } else if (quoi == "QUITTER") {
            break;
        } else {
            ecrire_ligne("ERREUR\tcommande_inconnue");
        }
    }

    // Fin d'entrée ou QUITTER : on rend la mémoire tout de suite.
    if (moteur != nullptr) {
        delete moteur;
        moteur = nullptr;
    }
    llama_backend_free();
    return 0;
}
