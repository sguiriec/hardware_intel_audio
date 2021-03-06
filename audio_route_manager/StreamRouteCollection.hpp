/*
 * Copyright (C) 2015-2017 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "AudioStreamRoute.hpp"
#include <Direction.hpp>
#include <IoStream.hpp>
#include <AudioUtilitiesAssert.hpp>
#include <utilities/Log.hpp>
#include <list>
#include <map>
#include <utils/String8.h>

namespace audio_hal
{

class StreamRouteCollection : public std::map<std::string, AudioStreamRoute *>
{
public:
    ~StreamRouteCollection()
    {
        reset();
    }

    void reset()
    {
        for (auto it : *this) {
            delete it.second;
        }
        (*this).clear();
    }

    /**
     * Reset the availability of stream route collection.
     */
    void resetAvailability()
    {
        for (uint32_t i = 0; i < Direction::gNbDirections; i++) {
            mRoutes[i].reset();
        }
        for (auto it : *this) {
            it.second->resetAvailability();
        }
    }

    /** Add a stream route.
     *
     * @param[in] element to be added.
     *
     * @return Newly created and added element, NULL otherwise..
     */
    void push_back(AudioStreamRoute *element)
    {
        std::string key = element->getName() + (element->isOut() ? "_Playback" : "_Capture");
        if ((*this).find(key) != (*this).end()) {
            audio_utilities::utilities::Log::Warning() << __FUNCTION__
                                                   << ": element(" << key << ") already added";
            return;
        }
        (*this)[key] = element;
    }

    void prepareRouting()
    {
        for (auto it : *this) {
            auto route = it.second;
            // The stream route collection must not only ensure that the route is applicable
            // but also that a stream matches the route.
            if (route && (not route->isUsed()) && setStreamForRoute(*route)) {
                route->setUsed();
                mRoutes[route->isOut()].setEnabledRoute(route->getMask());
                if (route->needReflow())
                    mRoutes[route->isOut()].setNeedReflowRoute(route->getMask());
                if (route->needRepath())
                    mRoutes[route->isOut()].setNeedRepathRoute(route->getMask());
            }
        }
    }

    bool routingHasChanged() const
    {
        return mRoutes[Direction::Output].routingHasChanged()
               || mRoutes[Direction::Input].routingHasChanged();
    }

    /**
     * Get the need repath routes mask.
     *
     * @return repath routes mask in the requested direction.
     */
    inline uint32_t needRepathRouteMask(Direction::Values dir) const
    {
        return mRoutes[dir].needRepathRoutes();
    }

    /**
     * Get the need repath routes mask.
     *
     * @return repath routes mask in the requested direction.
     */
    inline uint32_t needReflowRouteMask(Direction::Values dir) const
    {
        return mRoutes[dir].needReflowRoutes();
    }

    void addStream(IoStream &stream)
    {
        // Note: Priority shall be given to direct stream first when routing streams.
        if (stream.isDirect()) {
            mOrderedStreamList[stream.isOut()].push_front(&stream);
        } else {
            mOrderedStreamList[stream.isOut()].push_back(&stream);
        }
    }

    void removeStream(IoStream &streamToRemove)
    {
        mOrderedStreamList[streamToRemove.isOut()].remove(&streamToRemove);
    }

    /**
     * Find and set a stream for an applicable route.
     * It try to associate a streams that must be started and not already routed, with a stream
     * route according to the applicability mask.
     * This mask depends on the direction of the stream:
     *      -Output stream: output Flags
     *      -Input stream: input source.
     *
     * @param[in] route applicable route to be associated to a stream.
     *
     * @return true if a stream was found and attached to the route, false otherwise.
     */
    bool setStreamForRoute(AudioStreamRoute &route)
    {
        for (auto stream : mOrderedStreamList[route.isOut()]) {
            if (stream->isStarted() && stream->isRoutedByPolicy() &&
                    !stream->isNewRouteAvailable()) {
                if (route.isMatchingWithStream(*stream)) {
                    audio_utilities::utilities::Log::Verbose() << __FUNCTION__ << ": route "
                                                           << route.getName()
                                                           << " is maching with the stream";
                    return route.setStream(*stream);
                }
            }
        }
        return false;
    }

    IoStream *getVoiceStreamRoute()
    {
        // We take the first stream that corresponds to the primary output.
        auto it = mOrderedStreamList[Direction::Output].begin();
        if (*it == NULL) {
            audio_utilities::utilities::Log::Error() << __FUNCTION__
                                                 << ": current stream NOT FOUND for echo ref";
        }
        return *it;
    }

    /**
     * Performs the disabling of the route.
     * It only concerns the action that needs to be done on routes themselves, ie detaching
     * streams, closing alsa devices.
     * Disable Routes that were opened before reconsidering the routing and will be closed after
     * or routes that request to be rerouted.
     *
     * @param[in] bIsPostDisable if set, it indicates that the disable happens after unrouting.
     */
    void disableRoutes(bool isPostDisable = false)
    {
        for (auto it : *this) {
            auto route = it.second;

            if (route && ((route->previouslyUsed() && !route->isUsed()) || route->needRepath())) {
                audio_utilities::utilities::Log::Verbose() << __FUNCTION__
                                                       << ": Route " << route->getName()
                                                       << " to be disabled";
                route->unroute(isPostDisable);
            }
        }
    }

    /**
     * Performs the enabling of the routes.
     * It only concerns the action that needs to be done on routes themselves, ie attaching
     * streams, opening alsa devices.
     * Enable Routes that were not enabled and will be enabled after the routing reconsideration
     * or routes that requested to be rerouted.
     *
     * @tparam isOut direction of the routes to disable.
     * @param[in] bIsPreEnable if set, it indicates that the enable happens before routing.
     */
    void enableRoutes(bool isPreEnable = false)
    {
        for (auto it : *this) {
            auto route = it.second;

            if (route && ((!route->previouslyUsed() && route->isUsed()) || route->needRepath())) {
                audio_utilities::utilities::Log::Verbose() << __FUNCTION__
                                                       << ": Route" << route->getName()
                                                       << " to be enabled";
                if (route->route(isPreEnable) != android::OK) {
                    audio_utilities::utilities::Log::Error() << "\t error while routing "
                                                         << route->getName();
                }
            }
        }
    }

    /**
     * Find the most suitable route for a given stream according to its attributes, ie flags,
     * use cases, effects...
     *
     * @param[in] stream for which the matching route request is performed
     *
     * @return valid stream route if found, NULL otherwise.
     */
    const AudioStreamRoute *findMatchingRouteForStream(const IoStream &stream) const
    {
        for (const auto it : *this) {
            const auto streamRoute = it.second;
            if (streamRoute->isMatchingWithStream(stream)) {
                return streamRoute;
            }
        }
        return NULL;
    }

    /**
     * Handle the change of state of a device to whom it concerns by loading / resetting
     * capabilities of route(s) supporting this device.
     * @param[in] device that has been connected / disconnected
     * @param[in] state of the device.
     */
    void handleDeviceConnectionState(audio_devices_t device, bool isConnected)
    {
        for (auto it : *this) {
            auto route = it.second;
            if ((route->getSupportedDeviceMask() & device) == device) {
                if (isConnected) {
                    route->loadCapabilities();
                } else {
                    route->resetCapabilities();
                }
            }
        }
    }

    /**
     * Performs the post-disabling of the route.
     * It only concerns the action that needs to be done on routes themselves, ie detaching
     * streams, closing alsa devices. Some platform requires to close stream before unrouting.
     * Behavior is encoded in the route itself.
     *
     */
    inline void postDisableRoutes()
    {
        disableRoutes(true);
    }

    /**
     * Performs the pre-enabling of the routes.
     * It only concerns the action that needs to be done on routes themselves, ie attaching
     * streams, opening alsa devices. Some platform requires to open stream before routing.
     * Behavior is encoded in the route itself.
     */
    inline void preEnableRoutes()
    {
        enableRoutes(true);
    }

    /**
     * array of list of streams opened.
     */
    std::list<IoStream *> mOrderedStreamList[Direction::gNbDirections];

    /**
     * Get the enabled routes mask.
     *
     * @return enabled routes mask in the requested direction.
     */
    inline uint32_t enabledRouteMask(Direction::Values dir) const
    {
        return mRoutes[dir].enabledRoutes();
    }

    /**
     * Get the prevously enabled routes mask.
     *
     * @return previously enabled routes mask in the requested direction.
     */
    inline uint32_t prevEnabledRouteMask(Direction::Values dir) const
    {
        return mRoutes[dir].prevEnabledRoutes();
    }

    inline uint32_t unmutedRoutes(Direction::Values dir) const
    {
        return mRoutes[dir].unmutedRoutes();
    }

    inline uint32_t routesToMute(Direction::Values dir) const
    {
        return mRoutes[dir].routesToMute();
    }

    inline uint32_t openedRoutes(Direction::Values dir) const
    {
        return mRoutes[dir].openedRoutes();
    }

    inline uint32_t routesToDisable(Direction::Values dir) const
    {
        return mRoutes[dir].routesToDisable();
    }

    android::status_t dump(const int fd, int spaces) const
    {
        const size_t SIZE = 256;
        char buffer[SIZE];
        android::String8 result;

        snprintf(buffer, SIZE, "%*sStream Routes:\n", spaces, "");
        result.append(buffer);

        write(fd, result.string(), result.size());

        for (const auto &it : *this) {
            const auto &route = it.second;
            route->dump(fd, spaces + 4);
        }
        return android::OK;
    }

private:
    class RouteMasks
    {
    private:
        uint32_t needReflow;  /**< Bitfield of routes that needs to be mute / unmutes. */
        uint32_t needRepath;  /**< Bitfield of routes that needs to be disabled / enabled. */
        uint32_t enabled;     /**< Bitfield of enabled routes. */
        uint32_t prevEnabled; /**< Bitfield of previously enabled routes. */

    public:
        RouteMasks()
            : needReflow(0), needRepath(0), enabled(0), prevEnabled(0)
        {}

        inline void setEnabledRoute(uint32_t index)
        {
            setBit(index, enabled);
        }

        /**
         * Sets a bit referred by an index within a mask.
         *
         * @param[in] index bit index to set.
         * @param[in,out] mask in which the bit must be set.
         */
        inline void setBit(uint32_t index, uint32_t &mask)
        {
            mask |= index;
        }

        /**
         * Get the need reflow routes mask.
         *
         * @return reflow routes mask in the requested direction.
         */
        inline uint32_t needReflowRoutes() const
        {
            return needReflow;
        }

        inline void setNeedReflowRoute(uint32_t index)
        {
            setBit(index, needReflow);
        }

        /**
         * Get the enabled routes mask.
         *
         * @return enabled routes mask in the requested direction.
         */
        inline uint32_t enabledRoutes() const
        {
            return enabled;
        }

        inline void setNeedRepathRoute(uint32_t index)
        {
            setBit(index, needRepath);
        }

        /**
         * Get the need repath routes mask.
         *
         * @return repath routes mask in the requested direction.
         */
        inline uint32_t needRepathRoutes() const
        {
            return needRepath;
        }

        /**
         * Get the prevously enabled routes mask.
         *
         * @return previously enabled routes mask in the requested direction.
         */
        inline uint32_t prevEnabledRoutes() const
        {
            return prevEnabled;
        }

        /**
         * Checks if the routing conditions changed in a given direction.
         *
         * @tparam isOut direction of the route to consider.
         *
         * @return  true if previously enabled routes is different from currently enabled routes
         *               or if any route needs to be reconfigured.
         */
        bool routingHasChanged() const
        {
            return prevEnabledRoutes() != enabledRoutes()
                   || needReflowRoutes() != 0
                   || needRepathRoutes() != 0;
        }

        void reset()
        {
            prevEnabled = enabled;
            enabled = 0;
            needReflow = 0;
            needRepath = 0;
        }

        uint32_t unmutedRoutes() const
        {
            return prevEnabledRoutes() & enabledRoutes() & ~needReflowRoutes();
        }

        uint32_t routesToMute() const
        {
            return (prevEnabledRoutes() & ~enabledRoutes()) | needReflowRoutes();
        }

        uint32_t openedRoutes() const
        {
            return prevEnabledRoutes() & enabledRoutes() & ~needRepathRoutes();
        }

        uint32_t routesToDisable() const
        {
            return (prevEnabledRoutes() & ~enabledRoutes()) | needRepathRoutes();
        }
    } mRoutes[Direction::gNbDirections];
};

} // namespace audio_hal
