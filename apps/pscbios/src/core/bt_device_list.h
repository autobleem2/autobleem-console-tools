//
// BtDeviceList: what the Bluetooth pairing screen lists and in which state - the devices of the last scan plus the
// paired ones, each new / discovering... / pairing... / connecting... / paired / connected / dropped / failed,
// with its battery and, after a failure, why. The screen feeds it the scans, the periodic list of paired devices
// and the pairing's stages; "dropped" is a paired pad that was connected and no longer is (out of range, switched
// off, its battery flat). SDL-free, tested.
//
#pragma once

#include "console_backend.h"

#include <string>
#include <vector>

//******************
// BtRowState
//******************
enum class BtRowState { New, Discovering, Pairing, Connecting, Paired, Connected, Dropped, Failed };

//******************
// BtRow
//******************
struct BtRow {
    BtDevice device;
    BtRowState state = BtRowState::New;
    bool wasConnected = false; // seen connected since the screen opened (or paired) - a disconnect is "dropped"
    std::string error;         // why the last pairing or removal failed ("" after one that worked)
};

//******************
// BtDeviceList
//******************
class BtDeviceList {
public:
    const std::vector<BtRow> &rows() const { return rows_; }
    bool empty() const { return rows_.empty(); }
    int indexOf(const std::string &mac) const; // -1 when not listed
    const BtRow *find(const std::string &mac) const;

    // a scan's devices: new ones added, the names and states of known ones brought up to date; a device the scan
    // no longer sees stays while it is paired and goes otherwise
    void setScanned(const std::vector<BtDevice> &found);
    // the paired devices as BlueZ has them now (a periodic read): paired/connected/battery updated, a paired one
    // not listed yet added, a device no longer paired back to new; a pairing under way keeps its state
    void updatePaired(const std::vector<BtDevice> &paired);
    // the pairing of `mac`: its stage while it runs, then how it ended
    void pairStage(const std::string &mac, BtPairStage stage);
    void pairFinished(const std::string &mac, bool paired, bool connected, const std::string &error);
    void removed(const std::string &mac, bool ok, const std::string &error);

    // "new", "pairing...", "connected, battery 80%" ... translated - the row's value column
    static std::string stateText(const BtRow &row);
    static std::string batteryText(const BtBattery &battery); // "battery 80%" / "charging 80%", "" when unknown

private:
    BtRow *row(const std::string &mac);
    static BtRowState settledState(const BtRow &row); // from paired/connected, for a row with nothing under way
    static bool underWay(BtRowState state);

    std::vector<BtRow> rows_;
};
