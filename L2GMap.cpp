// Copyright (C) 2020 Chris Richardson (chris@bpi.cam.ac.uk)
// SPDX-License-Identifier:    LGPL-3.0-or-later

#include "L2GMap.h"
#include <algorithm>
#include <set>
#include <vector>

//-----------------------------------------------------------------------------
L2GMap::L2GMap(MPI_Comm comm, const std::vector<index_type>& ranges,
               const std::vector<index_type>& ghosts)
    : _ranges(ranges)
{
  int mpi_size;
  MPI_Comm_size(comm, &mpi_size);
  int mpi_rank;
  MPI_Comm_rank(comm, &mpi_rank);

  const index_type r0 = _ranges[mpi_rank];
  const index_type r1 = _ranges[mpi_rank + 1];
  const index_type local_size = r1 - r0;

  // Get count on each process
  std::vector<std::int32_t> ghost_count(mpi_size);
  for (index_type idx : ghosts)
  {
    if (idx >= r0 and idx < r1)
      throw std::runtime_error("Ghost index in local range");
    auto it = std::lower_bound(ranges.begin(), ranges.end(), idx);
    assert(it != ranges.end());
    ++ghost_count[*it];
  }

  std::vector<int> neighbours;
  for (std::size_t i = 0; i < ghost_count.size(); ++i)
  {
    const std::int32_t c = ghost_count[i];
    if (c > 0)
    {
      neighbours.push_back(i);
      _send_count.push_back(c);
    }
  }

  const int neighbour_size = neighbours.size();

  MPI_Dist_graph_create_adjacent(comm, neighbours.size(), neighbours.data(),
                                 MPI_UNWEIGHTED, neighbours.size(),
                                 neighbours.data(), MPI_UNWEIGHTED,
                                 MPI_INFO_NULL, false, &_neighbour_comm);

  // Send NNZs by Alltoall - these will be the receive counts for incoming
  // index/values
  _recv_count.resize(neighbour_size);
  MPI_Neighbor_alltoall(_send_count.data(), 1, MPI_INT, _recv_count.data(), 1,
                        MPI_INT, _neighbour_comm);

  _send_offset = {0};
  for (int c : _send_count)
    _send_offset.push_back(_send_offset.back() + c);

  _recv_offset = {0};
  for (int c : _recv_count)
    _recv_offset.push_back(_recv_offset.back() + c);
  int count = _recv_offset.back();

  _indexbuf.resize(count);
  _databuf.resize(count);

  // Send global indices to remote processes that own them
  int err = MPI_Neighbor_alltoallv(
      ghosts.data(), _send_count.data(), _send_offset.data(), MPI_INT,
      _indexbuf.data(), _recv_count.data(), _recv_offset.data(), MPI_INT,
      _neighbour_comm);
  if (err != MPI_SUCCESS)
    throw std::runtime_error("MPI failure");

  // Should be in own range, subtract off r0
  for (index_type& i : _indexbuf)
  {
    assert(i >= r0 and i < r1);
    i -= r0;
  }

  // Add local_range onto _send_offset
  for (index_type& s : _send_offset)
    s += local_size;
}
//-----------------------------------------------------------------------------
L2GMap::~L2GMap() { MPI_Comm_free(&_neighbour_comm); }
//-----------------------------------------------------------------------------
void L2GMap::update(double* vec_data)
{
  // Get data from local indices to send to other processes, landing in their
  // ghost region
  for (std::size_t i = 0; i < _indexbuf.size(); ++i)
    _databuf[i] = vec_data[_indexbuf[i]];

  // Send actual values - NB meaning of _send and _recv count/offset is
  // reversed
  int err = MPI_Neighbor_alltoallv(_databuf.data(), _recv_count.data(),
                                   _recv_offset.data(), MPI_DOUBLE, vec_data,
                                   _send_count.data(), _send_offset.data(),
                                   MPI_DOUBLE, _neighbour_comm);
  if (err != MPI_SUCCESS)
    throw std::runtime_error("MPI failure");
}
//-----------------------------------------------------------------------------
void L2GMap::reverse_update(double* vec_data)
{
  // Send values from ghost region of vector to remotes
  // accumulating in local vector.
  int err = MPI_Neighbor_alltoallv(
      vec_data, _send_count.data(), _send_offset.data(), MPI_DOUBLE,
      _databuf.data(), _recv_count.data(), _recv_offset.data(), MPI_DOUBLE,
      _neighbour_comm);
  if (err != MPI_SUCCESS)
    throw std::runtime_error("MPI failure");

  for (std::size_t i = 0; i < _indexbuf.size(); ++i)
    vec_data[_indexbuf[i]] += _databuf[i];
}
//-----------------------------------------------------------------------------