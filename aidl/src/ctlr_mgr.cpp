#include "ctlr_mgr.h"
#include "virt_ctlr_combined.h"
#include "virt_ctlr_passthrough.h"
#include "virt_ctlr_pro.h"

#include <iostream>
#include <unistd.h>
#include <utils/Log.h>

// private
void ctlr_mgr::epoll_event_callback(int event_fd) {
    for (auto &kv : unpaired_controllers) {
        auto ctlr = kv.second;
        if (event_fd == ctlr->get_fd()) {
            ctlr->handle_events();
            switch (ctlr->get_pairing_state()) {
            case phys_ctlr::PairingState::Lone:
                ALOGI("Lone controller paired");
                add_passthrough_ctlr(ctlr);
                break;
            case phys_ctlr::PairingState::Virt_Procon:
                ALOGI("Virtual procon paired");
                add_virt_procon_ctlr(ctlr);
                break;
            case phys_ctlr::PairingState::Waiting:
                ALOGI("Waiting controller needs partner");
                if (ctlr->get_model() == phys_ctlr::Model::Left_Joycon) {
                    if (!left) {
                        left = ctlr;
                        ALOGI("Found left");
                    }
                } else {
                    if (!right) {
                        right = ctlr;
                        ALOGI("Found right");
                    }
                }
                if (left && right) {
                    add_combined_ctlr();
                    left = nullptr;
                    right = nullptr;
                }
                break;
            case phys_ctlr::PairingState::Horizontal:
                ALOGI("Joy-Con paired in horizontal mode");
                add_passthrough_ctlr(ctlr);
                break;
            default:
                if (left == ctlr)
                    left = nullptr;
                if (right == ctlr)
                    right = nullptr;
                break;
            }
            break;
        }
    }

    for (auto &ctlr : paired_controllers) {
        if (!ctlr)
            continue;
        if (ctlr->contains_fd(event_fd))
            ctlr->handle_events(event_fd);
    }
}

void ctlr_mgr::add_passthrough_ctlr(std::shared_ptr<phys_ctlr> phys) {
    std::unique_ptr<virt_ctlr_passthrough> passthrough(
        new virt_ctlr_passthrough(phys));

    if (left == phys)
        left = nullptr;
    if (right == phys)
        right = nullptr;

    bool found_slot = false;
    for (unsigned int i = 0; i < paired_controllers.size(); i++) {
        if (!paired_controllers[i]) {
            found_slot = true;
            phys->set_player_leds_to_player(i % 4 + 1);
            paired_controllers[i] = std::move(passthrough);
            break;
        }
    }

    if (!found_slot) {
        phys->set_player_leds_to_player(paired_controllers.size() % 4 + 1);
        paired_controllers.push_back(std::move(passthrough));
    }

    unpaired_controllers.erase(phys->get_devpath());
}

void ctlr_mgr::add_combined_ctlr() {
    std::unique_ptr<virt_ctlr_combined> combined(
        new virt_ctlr_combined(left, right, epoll_manager, mMapping, mapLock));

    ALOGI("Creating combined joy-con input");

    bool found_slot = false;
    for (unsigned int i = 0; i < paired_controllers.size(); i++) {
        if (!paired_controllers[i]) {
            found_slot = true;
            left->set_player_leds_to_player(i % 4 + 1);
            right->set_player_leds_to_player(i % 4 + 1);
            combined->set_player_leds_to_player(i % 4 + 1);
            paired_controllers[i] = std::move(combined);
            break;
        }
    }
    if (!found_slot) {
        left->set_player_leds_to_player(paired_controllers.size() % 4 + 1);
        right->set_player_leds_to_player(paired_controllers.size() % 4 + 1);
        combined->set_player_leds_to_player(paired_controllers.size() % 4 + 1);
        paired_controllers.push_back(std::move(combined));
    }

    unpaired_controllers.erase(left->get_devpath());
    unpaired_controllers.erase(right->get_devpath());
}

void ctlr_mgr::add_virt_procon_ctlr(std::shared_ptr<phys_ctlr> phys) {
    std::unique_ptr<virt_ctlr_pro> procon(
        new virt_ctlr_pro(phys, epoll_manager, mMapping, mapLock));

    ALOGI("Creating virtual pro controller input");

    bool found_slot = false;
    for (unsigned int i = 0; i < paired_controllers.size(); i++) {
        if (!paired_controllers[i]) {
            found_slot = true;
            phys->set_player_leds_to_player(i % 4 + 1);
            procon->set_player_leds_to_player(i % 4 + 1);
            paired_controllers[i] = std::move(procon);
            break;
        }
    }
    if (!found_slot) {
        phys->set_player_leds_to_player(paired_controllers.size() % 4 + 1);
        procon->set_player_leds_to_player(paired_controllers.size() % 4 + 1);
        paired_controllers.push_back(std::move(procon));
    }

    unpaired_controllers.erase(phys->get_devpath());
}

// public
ctlr_mgr::ctlr_mgr(epoll_mgr &epoll_manager, struct mapping *mMapping,
                   pthread_mutex_t *mapLock)
    : epoll_manager(epoll_manager), unpaired_controllers(), subscribers(),
      paired_controllers() {
    this->mMapping = mMapping;
    this->mapLock = mapLock;
}

ctlr_mgr::~ctlr_mgr() {}

void ctlr_mgr::add_ctlr(const std::string &devpath,
                        const std::string &devname) {
    std::shared_ptr<phys_ctlr> phys = nullptr;

    if (!unpaired_controllers.count(devpath)) {
        ALOGI("Creating new phys_ctlr for %s", devname.c_str());
        phys.reset(new phys_ctlr(devpath, devname));
        unpaired_controllers[devpath] = phys;
        phys->blink_player_leds();
        subscribers[devpath] = std::make_shared<epoll_subscriber>(
            std::vector({phys->get_fd()}),
            [=](int event_fd) { epoll_event_callback(event_fd); });
        epoll_manager.add_subscriber(subscribers[devpath]);
    } else {
        ALOGE("Attempting to add existing phys_ctlr to controller manager");
        return;
    }

    // See if this controller belongs to a "stale" controller
    for (unsigned int i = 0; i < stale_controllers.size(); i++) {
        auto &virt = stale_controllers[i];

        if (!virt)
            continue;

        if (virt->mac_belongs(phys->get_mac_addr())) {
            bool found_slot = false;
            ALOGI("Re-pairing stale controller");
            for (unsigned int i = 0; i < paired_controllers.size(); i++) {
                if (!paired_controllers[i]) {
                    found_slot = true;
                    paired_controllers[i] = std::move(virt);
                    break;
                }
            }
            if (!found_slot)
                paired_controllers.push_back(std::move(virt));
            stale_controllers.erase(stale_controllers.begin() + i);
            break;
        }
    }

    // Check if a controller with this MAC already exists in a combined
    // controller
    for (unsigned int i = 0; i < paired_controllers.size(); i++) {
        auto &virt = paired_controllers[i];

        if (!virt)
            continue;

        if (virt->supports_hotplug()) {
            bool found;
            for (auto phys2 : virt->get_phys_ctlrs()) {
                if (phys->get_mac_addr() == phys2->get_mac_addr() &&
                    phys->get_mac_addr() != "") {
                    ALOGI(
                        "Replacing controller (likely a BT to serial switch)");
                    if (subscribers.count(phys2->get_devpath())) {
                        epoll_manager.remove_subscriber(
                            subscribers[phys2->get_devpath()]);
                        subscribers.erase(phys2->get_devpath());
                    }
                    virt->remove_phys_ctlr(phys2);
                    phys->set_player_leds_to_player(i % 4 + 1);
                    virt->add_phys_ctlr(phys);
                    unpaired_controllers.erase(phys->get_devpath());
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
    }

    // Now check if this is a reconnecting joy-con
    for (unsigned int i = 0; i < paired_controllers.size(); i++) {
        auto &virt = paired_controllers[i];

        if (!virt)
            continue;

        if (((virt->needs_model() == phys->get_model() &&
              phys->get_model() != phys_ctlr::Model::Unknown) ||
             virt->no_ctlrs_left()) &&
            virt->supports_hotplug()) {
            ALOGI("Detected reconnected joy-con");
            phys->set_player_leds_to_player(i % 4 + 1);
            virt->add_phys_ctlr(phys);
            unpaired_controllers.erase(phys->get_devpath());
        }
    }
    // check if we're already ready to pair this contoller
    if (unpaired_controllers.count(devpath))
        epoll_event_callback(unpaired_controllers[devpath]->get_fd());
}

void ctlr_mgr::remove_ctlr(const std::string &devpath) {
    if (subscribers.count(devpath)) {
        epoll_manager.remove_subscriber(subscribers[devpath]);
        subscribers.erase(devpath);
    }
    if (unpaired_controllers.count(devpath)) {
        ALOGI("Removing %s from unpaired list", devpath.c_str());
        auto phys = unpaired_controllers[devpath];
        if (phys == left)
            left = nullptr;
        if (phys == right)
            right = nullptr;
        unpaired_controllers.erase(devpath);
    }
    for (auto &ctlr : paired_controllers) {
        if (!ctlr)
            continue;
        bool found = false;
        for (auto phys : ctlr->get_phys_ctlrs()) {
            if (phys->get_devpath() == devpath) {
                bool serial = phys->is_serial_ctlr();

                if (ctlr->supports_hotplug())
                    ctlr->remove_phys_ctlr(phys);

                if (ctlr->no_ctlrs_left()) {
                    if (serial) {
                        ALOGI("Both serial joy-cons disconnected; keep ctlr "
                              "alive");
                        stale_controllers.push_back(std::move(ctlr));
                    } else {
                        ALOGI("unpairing controller");
                    }
                    ctlr = nullptr;
                }

                found = true;
                break;
            }
        }
        if (found)
            break;
    }
}
