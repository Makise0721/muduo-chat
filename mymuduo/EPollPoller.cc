#include "EPollPoller.h"

#include "Channel.h"
#include "EventLoop.h"
#include "Logger.h"

#include <unistd.h>
#include <cstring>
#include <errno.h>

const int knew = -1;
const int kadded = 1;
const int kdeleted = 2;

EPollPoller::EPollPoller(EventLoop *loop)
    : Poller(loop),
      epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
      events_(kInitEventListSize)
{
    if (epollfd_ < 0)
    {
        LOG_FATAL("EPollPoller::EPollPoller epoll_create1 error");
    }
}

EPollPoller::~EPollPoller()
{
    ::close(epollfd_);
}

void EPollPoller::updateChannel(Channel *channel)
{
    const int index = channel->index();
    LOG_INFO("updateChannel:fd=%d events=%d index=%d", channel->fd(), channel->events(), index);

    if (index == knew || index == kdeleted)
    {
        if (index == knew)
        {
            int fd = channel->fd();
            channels_[fd] = channel;
        }
        channel->set_index(kadded);
        update(EPOLL_CTL_ADD, channel);
    }
    else
    {
        int fd = channel->fd();
        if (channel->isNoneEvent())
        {
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(kdeleted);
        }
        else
        {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}

void EPollPoller::removeChannel(Channel *channel)
{
    int fd = channel->fd();
    LOG_INFO("removeChannel: fd=%d", fd);

    channels_.erase(fd);

    int index = channel->index();
    if (index == kadded)
    {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->set_index(knew);
}

Timestamp EPollPoller::poll(int timeoutMs, ChannelList *activeChannels)
{
    LOG_DEBUG("EPollPoller::poll() called");

    int numEvents = ::epoll_wait(epollfd_,
                                 events_.data(),
                                 static_cast<int>(events_.size()),
                                 timeoutMs);
    int savedErrno = errno;
    Timestamp now = Timestamp::now();
    if (numEvents > 0)
    {
        LOG_DEBUG("EPollPoller::poll() %d events happened", numEvents);
        fillActiveChannels(numEvents, activeChannels);
        if (static_cast<size_t>(numEvents) == events_.size())
        {
            events_.resize(events_.size() * 2);
        }
    }
    else if (numEvents == 0)
    {
        LOG_DEBUG("EPollPoller::poll() nothing happened");
    }
    else
    {
        if (savedErrno != EINTR)
        {
            errno = savedErrno;
            LOG_ERROR("EPollPoller::poll() error");
        }
    }
    return now;
}

void EPollPoller::update(int operation, Channel *channel)
{
    epoll_event event;
    bzero(&event, sizeof event);
    event.events = static_cast<uint32_t>(channel->events());
    event.data.ptr = channel;
    int fd = channel->fd();
    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0)
    {
        if (operation == EPOLL_CTL_DEL)
        {
            LOG_ERROR("EPollPoller::update() EPOLL_CTL_DEL error");
        }
        else
        {
            LOG_FATAL("EPollPoller::update() epoll_ctl error");
        }
    }
}

void EPollPoller::fillActiveChannels(int numEvents, ChannelList *activeChannels) const
{
    for (int i = 0; i < numEvents; ++i)
    {
        Channel *channel = static_cast<Channel *>(events_[i].data.ptr);
        channel->set_revents(static_cast<int>(events_[i].events));
        activeChannels->push_back(channel);
    }
}