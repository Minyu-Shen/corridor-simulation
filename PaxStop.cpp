//
//  PaxStop.cpp
//  Corridor-Simulation
//
//  Created by samuel on 7/26/18.
//  Copyright © 2018 samuel. All rights reserved.
//

#include "PaxStop.hpp"
#include "Queues.hpp"
#include <iostream>
#include "Bus.hpp"
#include "Link.hpp"

PaxStop::PaxStop(int sd, int bh_sz, const std::map<int, double> ldm, EnteringTypes eType, QueuingRules qRule, double cp_ratio, const std::map<int, int> lineGroupAMap){
    stopID = sd; berthSize = bh_sz; enterType = eType; common_ratio = cp_ratio;
    nextLink = nullptr; lineGroupAssignMap = lineGroupAMap;
    busesInStop.resize(bh_sz); servicingMark.resize(bh_sz);
    std::fill(servicingMark.begin(), servicingMark.end(), false);
    std::fill(busesInStop.begin(), busesInStop.end(), nullptr);
    
    int L = (int)ldm.size();
    groupLineSize = L / bh_sz;
    
    // (unshared) uncommon pax demand, for each line
    std::map<int, double>uncommonDemandMap; // line -> lambda
    double oneLineDemand = 0.0;
    for (auto &map: ldm){
        lines.push_back(map.first);
        uncommonDemandMap[map.first] = map.second * (1-common_ratio);
        if (map.first == 0) oneLineDemand = map.second;
    }
    // create common pax demand, for each group
    std::map<int, double>commonDemandMap; // group -> lambda
    for (int m_it = 0; m_it < bh_sz; m_it++) {
        commonDemandMap[m_it] = oneLineDemand * common_ratio * groupLineSize;
    }
    commonPaxQueue = std::make_shared<Queues>(commonDemandMap);
    uncommonPaxQueues = std::make_shared<Queues>(uncommonDemandMap);
    
    std::fill(servicingMark.begin(), servicingMark.end(), false);
    
    // busesInWaitzone  initialization?
    if (qRule == QueuingRules::FIFO || qRule == QueuingRules::LimitedOvertaking) isOvertakeIn = false;
    else isOvertakeIn = true;
    
    if (qRule == QueuingRules::LimitedOvertaking || qRule == QueuingRules::FreeOvertaking || qRule == QueuingRules::FreeOvertakingWithBlock) isOvertakeOut = true;
    else isOvertakeOut = false;
    stopDelays = 0.0;
    simTimeNow = 0.0;
}

void PaxStop::reset(){
    simTimeNow = 0.0;
    uncommonPaxQueues->reset();
    commonPaxQueue->reset();
    std::fill(busesInStop.begin(), busesInStop.end(), nullptr);
    busesInWaitzone.clear();
    std::fill(servicingMark.begin(), servicingMark.end(), false);
    stopDelays = 0.0;
}


// for the allocation case
void PaxStop::addAllocationPlan(std::map<int, int> allocPlan){
    if (enterType == EnteringTypes::Allocation) {
        // initialze allocationPlan
        for(auto &m: allocPlan){
            allocationPlan.insert(std::make_pair(m.first, m.second));
        }
    }else{
        std::cout << "Something wrong, stop type is not allocation" << std::endl;
    }
}

// 0 and 1 is called in the main function

// 0. pax arrivals
void PaxStop::paxArrival(){
    uncommonPaxQueues->arrival();
    commonPaxQueue->arrival();
}

// 1. buses arrivals, from last link
void PaxStop::busArrival(std::shared_ptr<Bus> bus){
    // record the bus has arrived at this stop
    bus->isEnterEachStop[stopID] = true;
    
    // record the current pax no.
    bus->paxNoEachStop[stopID] = bus->kPax;
    // record the bus arrival time here
    bus->arrivalTimeEachStop[stopID] = simTimeNow;
    
    // set the "alightingPaxEachStop"
    bus->determineAlightingPaxNo(bus->busLine);
    // check whether need to alight
    
    if (bus->alightingPaxEachStop > 0) { // some pax want to alight
        // first put bus in the waitZone
        // then at same simulation delta, check if it can proceed to stop
        busesInWaitzone.push_back(bus);
        
    } else { // no pax wants to alight
        // check if there is a same line serving or waiting in the stop
        bool isFindSameLine = false;
        for (auto &busInStop: busesInStop){
            if (busInStop == nullptr) continue;
            if (busInStop->busLine == bus->busLine){
                isFindSameLine = true;
                break;
            }
        }
        if (!isFindSameLine) {
            for (auto &busInWaitZone: busesInWaitzone){
                if (busInWaitZone->busLine == bus->busLine) {
                    isFindSameLine = true;
                    break;
                }
            }
        }
        if (isFindSameLine) { // leave the stop directly
            if (nextLink == nullptr) { // finally finished!
//                std::cout << bus->busID << " bus's leaving corridor : " << simTimeNow << "by skipping the last stop" << std::endl;
            } else{
                nextLink->busEnteringLink(bus);
            }
        }else{
            busesInWaitzone.push_back(bus); // enter the stop tamely -, -
        }
    }
}

// 2. buses entering
void PaxStop::entering(){
    if (!busesInWaitzone.empty()) {
        if (enterType == EnteringTypes::Normal) normalEntering();
        else if(enterType == EnteringTypes::Allocation) allocationEntering();
        else std::cout << "Entering type is wrong" << std::endl;
    }
}

void PaxStop::normalEntering(){
    while (!busesInWaitzone.empty()) {
        auto bus = busesInWaitzone.front();
        if (isOvertakeIn) {
            // just check the most-downstream empty berth
            int mostDownStreamBerthEmpty = -1;
            for (int c = berthSize-1; c >= 0; c--) {
                if (servicingMark[c] == false){ // as long as found, break;
                    mostDownStreamBerthEmpty = c;
                    break;
                }
            }
            if (mostDownStreamBerthEmpty >= 0) { // the berths are not all serving
                pushBusToBerth(bus, mostDownStreamBerthEmpty);
            }else{
                // no empty berth, break the while
                break;
            }
        } else{
            // check if can enter without overtake-in
            int berthAvailable = -1;
            for (int c = 0; c < berthSize; c++) {
                if (servicingMark[c] == true) break;
                else berthAvailable = c;
            }
            if (berthAvailable >= 0) { // at least one berth is empty
                pushBusToBerth(bus, berthAvailable);
            }else{
                // no empty berth, break the while
                break;
            }
        }
    }
}

void PaxStop::allocationEntering(){
    while (!busesInWaitzone.empty()) {
        auto bus = busesInWaitzone.front();
        int targetBerthNo = allocationPlan[bus->busLine];
        if (!servicingMark[targetBerthNo]){ // the target berth is empty
            bool isUpstreamBlockded = false;
            for (int c=0; c<targetBerthNo; c++) {
                if (servicingMark[c] == true){
                    isUpstreamBlockded = true; break;
                }
            }
            if (isUpstreamBlockded) { // find true, i.e., upstream berths are not all empty
                if (isOvertakeIn) {
                    // push the bus into the berth first, and then count the lost time
                    pushBusToBerth(bus, targetBerthNo);
                    std::cout << bus->busID << "entering time:" << simTimeNow << std::endl;
                }else{
                    break; // wait at waiting zone, do nothing
                }
            }else{ // can reach the target berth freely
                pushBusToBerth(bus, targetBerthNo);
                std::cout << bus->busID << "entering time:" << simTimeNow << std::endl;
            }
        }else{ // the target berth is full
            break; // wait at waiting zone, do nothing
        }
    }
}


void PaxStop::pushBusToBerth(std::shared_ptr<Bus> bus, int bth_no){
    bus->lostTime = 8.0;
    busesInStop[bth_no] = bus;
    busesInWaitzone.pop_front();
    servicingMark[bth_no] = true;
}

// 3. pax boarding and alighting
void PaxStop::paxOnOff(){
    // loop the busesInStop to board
    for (auto &bus: busesInStop){
        if (bus == nullptr) continue;
        if (bus->lostTime <= 0.0) { // acc/dec finished
            int ln = bus->busLine;
            // alighting ...
            bus->alighting(ln);
            
            // boarding ...
            int group = lineGroupAssignMap[ln];
            double commonPaxOnStop = commonPaxQueue->query(group);
            double uncommonPaxOnStop = uncommonPaxQueues->query(ln);
            if (commonPaxOnStop > 0) {
                if (uncommonPaxOnStop > 0) {
                    int rd_value = rand() % (2);
                    if (rd_value == 0) { // load common
                        double actualCommonPaxBoard = bus->boarding(group, commonPaxOnStop);
                        commonPaxQueue->decrease(group, actualCommonPaxBoard);
                    }else{ // load uncommon
                        double actualUnCommonPaxBoard = bus->boarding(ln, uncommonPaxOnStop);
                        uncommonPaxQueues->decrease(ln, actualUnCommonPaxBoard);
                    }
                }else{ // only common
                    double actualCommonPaxBoard = bus->boarding(group, commonPaxOnStop);
                    commonPaxQueue->decrease(group, actualCommonPaxBoard);
                }
            }else{
                if (uncommonPaxOnStop > 0) { // only uncommon
                    double actualUnCommonPaxBoard = bus->boarding(ln, uncommonPaxOnStop);
                    uncommonPaxQueues->decrease(ln, actualUnCommonPaxBoard);
                }else{ // no any pax
                    // do nothing
                }
            }
            
            
//            // boarding ...
//            // search all the other lines (that can be loaded)
//            std::vector<int> allCommonLines;
//            int group = lineGroupAssignMap[ln];
//
//            for (auto ln_it:lines){
//                if (lineGroupAssignMap[ln_it] == group) {
//                    allCommonLines.push_back(ln_it);
//                }
//            }
//            // store how many queues (that have pax)
//            std::map<int, double>allCommonLinePaxMap;
//            for (auto ln_it: allCommonLines){
//                double commonPaxOnStop_it =  commonPaxQueue->query(ln_it);
//                if (commonPaxOnStop_it > 0) {
//                    allCommonLinePaxMap[ln_it] = commonPaxOnStop_it;
//                }
//            }
//            // boarding ...
//            int totalCommonHavePax = (int)allCommonLinePaxMap.size();
//            double uncommonPaxOnStop = uncommonPaxQueues->query(ln);
//            if (uncommonPaxOnStop > 0) {
//                int rd_value = rand() % (totalCommonHavePax+1);
//                if (rd_value == totalCommonHavePax) { // unique uncommon queue
//                    double actualUncommonBoardPax = bus->boarding(ln, uncommonPaxOnStop);
//                    uncommonPaxQueues->decrease(ln, actualUncommonBoardPax);
//
//                }else{ // common queues
//                    if (totalCommonHavePax == 0) continue;
//                    auto it = allCommonLinePaxMap.begin();
//                    std::advance(it, rand() % allCommonLinePaxMap.size());
//                    int random_ln = it->first;
//                    double commonPaxOnStop = allCommonLinePaxMap[random_ln];
//                    // for now, just treat all the onboard pax as the same; i.e. one line
//                    double actualCommonBoardPax = bus->boarding(ln, commonPaxOnStop);
//                    commonPaxQueue->decrease(random_ln, actualCommonBoardPax);
//                }
//            }else{ // only common queues
//                if (totalCommonHavePax == 0) continue;
//                auto it = allCommonLinePaxMap.begin();
//                std::advance(it, rand() % (int)allCommonLinePaxMap.size());
//                int random_ln = it->first;
//                double commonPaxOnStop = allCommonLinePaxMap[random_ln];
//
//                // for now, just treat all the onboard pax as the same; i.e. one line
//                double actualCommonBoardPax = bus->boarding(ln, commonPaxOnStop);
//
//                if (stopID == 0 && bus->busID==0) {
//                    std::cout << simTimeNow <<":" << bus->remainSpace() << ":" << actualCommonBoardPax <<std::endl;
//                }
//
//                commonPaxQueue->decrease(random_ln, actualCommonBoardPax);
//            }
        } else{ // still acc/dec
            bus->lostTime -= 1.0;
        }
    }
}


// 4. buses leaving
void PaxStop::leaving(){
    for (int i = int(busesInStop.size())-1; i >= 0; i--) {
        if (busesInStop[i] == nullptr) continue;
        if (boardingAlightingCompleted(busesInStop[i])) {
            if (canLeave(i)) {
                if (nextLink == nullptr) { // finally finished!
                    
                } else{
                    nextLink->busEnteringLink(busesInStop[i]);
                }
                busesInStop[i] = nullptr;
                servicingMark[i] = false;
            }else{
                // just wait
            }
        }
    }
}

bool PaxStop::canLeave(int berthNo){
    if (isOvertakeOut) return true;
    if (berthNo == berthSize-1) {
        return true;
    }else{
        // check if all the downstream berths are empty
        for (int c = berthNo+1; c < berthSize; c++) {
            if (servicingMark[c] == true) return false;
        }
        return true;
    }
}

bool PaxStop::boardingAlightingCompleted(std::shared_ptr<Bus> bus){
    if (bus->alightingPaxEachStop > 0) return false;
    int ln = bus->busLine;
    // check if this unique uncommon line have pax
    if (uncommonPaxQueues->query(ln) > 0 && bus->remainSpace() > 0) return false;
    // check if the other common lines have pax
    int group = lineGroupAssignMap[ln];
    if (commonPaxQueue->query(group) > 0 && bus->remainSpace() > 0) return false;
    
//    for (auto ln_it:lines){
//        if (lineGroupAssignMap[ln_it] == group) {
//            if (commonPaxQueue->query(ln_it) > 0 && bus->remainSpace() > 0) return false;
//        }
//    }
    return true;
}

void PaxStop::operation(){
    entering();
    paxOnOff();
    leaving();
    simTimeNow += 1.0;
}

void PaxStop::paxDemandBounding(double d){
//    commonPaxQueue->paxDemandBounding(common_ratio*d);
    commonPaxQueue->paxDemandBounding(common_ratio * d * groupLineSize);
    uncommonPaxQueues->paxDemandBounding((1-common_ratio) * d);
}

// for stats
void PaxStop::updateBusStats(){
    for (int i = 0; i < int(busesInStop.size()); i++) {
        auto bus = busesInStop[i];
        if (bus == nullptr) continue;
        if (bus->isPeak) {
            if (boardingAlightingCompleted(bus)) {
                if (!canLeave(i)) {
                    bus->delayAtEachStop[stopID] += 1.0;
                    bus->exitDelayEachStop[stopID] += 1.0;
                }
            }else{ // still serving, for recording effective service time
                bus->serviceTimeAtEachStop[stopID] += 1.0;
            }
        }
    }
//    if (stopID==0) {
//        std::cout <<busesInWaitzone.size() <<std::endl;
//    }
    for (int i = int(busesInWaitzone.size())-1; i >= 0; i--) {
        auto bus = busesInWaitzone[i];
        if (bus->isPeak) {
            bus->delayAtEachStop[stopID] += 1.0;
            bus->entryDelayEachStop[stopID] += 1.0;
        }
    }
}

void PaxStop::writeToJson(nlohmann::json &j, double time){
    std::string timeString = std::to_string(time);
    
    for (int c = 0; c < busesInStop.size(); c++) {
        auto bus = busesInStop[c];
        if (bus == nullptr) continue;
        std::string busIDStr = std::to_string(bus->busID);
        j["buses"][busIDStr][timeString]["link_id"] = -1; // means not on the link
        j["buses"][busIDStr][timeString]["stop_id"] = stopID;
        j["buses"][busIDStr][timeString]["berth_id"] = c;
        if (!boardingAlightingCompleted(bus)) {
            j["buses"][busIDStr][timeString]["is_serving"] = true;
        }else{
            j["buses"][busIDStr][timeString]["is_serving"] = false;
        }
        j["buses"][busIDStr][timeString]["line_no"] = bus->busLine;
    }
    
    // write the buses in the waitZone into the json
    for (int c = 0; c < busesInWaitzone.size(); c++) {
        auto bus = busesInWaitzone[c];
        if (bus == nullptr) continue;
        std::string busIDStr = std::to_string(bus->busID);
        j["buses"][busIDStr][timeString]["link_id"] = -1; // means not on the link
        j["buses"][busIDStr][timeString]["stop_id"] = stopID;
        j["buses"][busIDStr][timeString]["berth_id"] = -1; // means not in the berth
        j["buses"][busIDStr][timeString]["wait_position"] = c;
        j["buses"][busIDStr][timeString]["line_no"] = bus->busLine;
    }
    
    // write the paxqueues of each stop into the json
    for (auto des:lines){
        double pax = uncommonPaxQueues->query(des);
//        std::string lineIndicator = std::to_string("line");
        std::string lineIDStr = std::to_string(des);
        std::string stopIDStr = std::to_string(stopID);
        j["pax_queues"][lineIDStr][timeString][stopIDStr] = pax;
    }
}





