

#include "Matrix.h"
#include "L2GMap.h"
#include <iostream>
#include <numeric>

using namespace spmv;

Matrix::Matrix(Eigen::SparseMatrix<double, Eigen::RowMajor> A,
               std::shared_ptr<spmv::L2GMap> col_map)
    : _matA(A), _col_map(col_map)
{
  // _row_map = std::make_shared<L2GMap>(comm, _matA.rows(), {});

#ifdef EIGEN_USE_MKL_ALL
  sparse_status_t status = mkl_sparse_d_create_csr(
      &A_mkl, SPARSE_INDEX_BASE_ZERO, _matA.rows(), _matA.cols(),
      _matA.outerIndexPtr(), _matA.outerIndexPtr() + 1, _matA.innerIndexPtr(),
      _matA.valuePtr());
  assert(status == SPARSE_STATUS_SUCCESS);

  status = mkl_sparse_optimize(A_mkl);
  assert(status == SPARSE_STATUS_SUCCESS);

  if (status != SPARSE_STATUS_SUCCESS)
    throw std::runtime_error("Could not create MKL matrix");

  mat_desc.type = SPARSE_MATRIX_TYPE_GENERAL;
  mat_desc.diag = SPARSE_DIAG_NON_UNIT;
#endif
}
//-----------------------------------------------------------------------------
Eigen::VectorXd Matrix::operator*(const Eigen::VectorXd& b) const
{
#ifdef EIGEN_USE_MKL_ALL
  Eigen::VectorXd y(_matA.rows());
  mkl_sparse_d_mv(SPARSE_OPERATION_NON_TRANSPOSE, 1.0, A_mkl, mat_desc,
                  b.data(), 0.0, y.data());

  return y;
#else
  return _matA * b;
#endif
}
//-----------------------------------------------------------------------------
Eigen::VectorXd Matrix::transpmult(const Eigen::VectorXd& b) const
{
#ifdef EIGEN_USE_MKL_ALL
  Eigen::VectorXd y(_matA.cols());
  mkl_sparse_d_mv(SPARSE_OPERATION_TRANSPOSE, 1.0, A_mkl, mat_desc, b.data(),
                  0.0, y.data());

  return y;
#else
  return _matA.transpose() * b;
#endif
}
//-----------------------------------------------------------------------------
Matrix Matrix::create_matrix(
    MPI_Comm comm, const Eigen::SparseMatrix<double, Eigen::RowMajor> mat,
    std::int64_t nrows_local, std::int64_t ncols_local,
    std::vector<std::int64_t> row_ghosts, std::vector<std::int64_t> col_ghosts)
{

  int mpi_size, mpi_rank;
  MPI_Comm_size(comm, &mpi_size);
  MPI_Comm_rank(comm, &mpi_rank);

  std::vector<std::int64_t> row_ranges(mpi_size + 1, 0);
  MPI_Allgather(&nrows_local, 1, MPI_INT64_T, row_ranges.data() + 1, 1,
                MPI_INT64_T, comm);
  for (int i = 0; i < mpi_size; ++i)
    row_ranges[i + 1] += row_ranges[i];

  // FIX: often same as rows?
  std::vector<std::int64_t> col_ranges(mpi_size + 1, 0);
  MPI_Allgather(&ncols_local, 1, MPI_INT64_T, col_ranges.data() + 1, 1,
                MPI_INT64_T, comm);
  for (int i = 0; i < mpi_size; ++i)
    col_ranges[i + 1] += col_ranges[i];

  // send all ghost rows to their owners, using global col idx.
  const std::int32_t* Aouter = mat.outerIndexPtr();
  const std::int32_t* Ainner = mat.innerIndexPtr();
  const double* Aval = mat.valuePtr();

  std::vector<std::vector<std::int64_t>> p_to_nnz(mpi_size);
  std::vector<std::vector<double>> p_to_val(mpi_size);
  for (std::size_t i = 0; i < row_ghosts.size(); ++i)
  {
    int ghost_idx = row_ghosts[i];
    auto it = std::upper_bound(row_ranges.begin(), row_ranges.end(), ghost_idx);
    assert(it != row_ranges.end());
    const int p = it - row_ranges.begin() - 1;
    p_to_nnz[p].push_back(ghost_idx);
    p_to_val[p].push_back(0.0);
    p_to_nnz[p].push_back(Aouter[i + 1] - Aouter[i]);
    p_to_val[p].push_back(0.0);

    const std::int64_t local_offset = col_ranges[mpi_rank];
    for (int j = Aouter[i]; j < Aouter[i + 1]; ++j)
    {
      std::int64_t global_index;
      if (Ainner[j] < ncols_local)
        global_index = Ainner[j] + local_offset;
      else
      {
        assert(Ainner[j] - ncols_local < (int)col_ghosts.size());
        global_index = col_ghosts[Ainner[j] - ncols_local];
      }
      p_to_nnz[p].push_back(global_index);
      p_to_val[p].push_back(Aval[j]);
    }
  }

  // Create a neighbour comm?

  std::vector<int> send_size(mpi_size);
  std::vector<std::int64_t> send_index;
  std::vector<double> send_val;
  std::vector<int> send_offset = {0};
  std::vector<int> recv_size(mpi_size);
  for (int p = 0; p < mpi_size; ++p)
  {
    send_index.insert(send_index.end(), p_to_nnz[p].begin(), p_to_nnz[p].end());
    send_val.insert(send_val.end(), p_to_val[p].begin(), p_to_val[p].end());
    assert(p_to_val[p].size() == p_to_nnz[p].size());
    send_size[p] = p_to_nnz[p].size();
    send_offset.push_back(send_index.size());
  }

  MPI_Alltoall(send_size.data(), 1, MPI_INT, recv_size.data(), 1, MPI_INT,
               comm);

  std::vector<int> recv_offset = {0};
  for (int p = 0; p < mpi_size; ++p)
    recv_offset.push_back(recv_offset.back() + recv_size[p]);

  std::vector<std::int64_t> recv_index(recv_offset.back());
  std::vector<double> recv_val(recv_offset.back());

  MPI_Alltoallv(send_index.data(), send_size.data(), send_offset.data(),
                MPI_INT64_T, recv_index.data(), recv_size.data(),
                recv_offset.data(), MPI_INT64_T, comm);

  MPI_Alltoallv(send_val.data(), send_size.data(), send_offset.data(),
                MPI_DOUBLE, recv_val.data(), recv_size.data(),
                recv_offset.data(), MPI_DOUBLE, comm);

  // Create new map from global column index to local
  std::map<std::int64_t, int> col_ghost_map;
  for (std::int64_t& q : col_ghosts)
    col_ghost_map.insert({q, -1});

  // Add any new ghost columns
  int pos = 0;
  while (pos < (int)recv_index.size())
  {
    //    std::int64_t global_row = recv_index[pos];
    ++pos;
    int nnz = recv_index[pos];
    ++pos;
    for (int k = 0; k < nnz; ++k)
    {
      const std::int64_t recv_col = recv_index[pos];
      ++pos;
      if (recv_col >= col_ranges[mpi_rank + 1]
          or recv_col < col_ranges[mpi_rank])
        col_ghost_map.insert({recv_col, -1});
    }
  }

  // Unique numbering of ghost cols
  int c = ncols_local;
  for (auto& q : col_ghost_map)
    q.second = c++;

  std::vector<Eigen::Triplet<double>> mat_data;

  for (int row = 0; row < nrows_local; ++row)
    for (int j = Aouter[row]; j < Aouter[row + 1]; ++j)
    {
      int col = Ainner[j];
      if (col > nrows_local)
      {
        // Get remapped ghost column
        std::int64_t global_col = col_ghosts[col - nrows_local];
        auto it = col_ghost_map.find(global_col);
        assert(it != col_ghost_map.end());
        col = it->second;
      }
      assert(row >= 0 and row < nrows_local);
      assert(col >= 0 and col < (int)(ncols_local + col_ghost_map.size()));
      mat_data.push_back(Eigen::Triplet<double>(row, col, Aval[j]));
    }

  // Add received data
  pos = 0;
  while (pos < (int)recv_index.size())
  {
    std::int64_t global_row = recv_index[pos];
    assert(global_row >= row_ranges[mpi_rank]
           and global_row < row_ranges[mpi_rank + 1]);
    std::int32_t row = global_row - row_ranges[mpi_rank];
    ++pos;
    int nnz = recv_index[pos];
    ++pos;
    for (int k = 0; k < nnz; ++k)
    {
      const std::int64_t global_col = recv_index[pos];
      const double val = recv_val[pos];
      ++pos;
      int col;
      if (global_col >= col_ranges[mpi_rank + 1]
          or global_col < col_ranges[mpi_rank])
      {
        auto it = col_ghost_map.find(global_col);
        assert(it != col_ghost_map.end());
        col = it->second;
      }
      else
        col = global_col - col_ranges[mpi_rank];
      assert(row >= 0 and row < nrows_local);
      assert(col >= 0 and col < (int)(ncols_local + col_ghost_map.size()));
      mat_data.push_back(Eigen::Triplet<double>(row, col, val));
    }
  }

  // Rebuild sparse matrix
  std::vector<std::int64_t> new_col_ghosts;
  for (auto& q : col_ghost_map)
    new_col_ghosts.push_back(q.first);

  Eigen::SparseMatrix<double, Eigen::RowMajor> B(
      nrows_local, ncols_local + new_col_ghosts.size());
  B.setFromTriplets(mat_data.begin(), mat_data.end());

  std::shared_ptr<spmv::L2GMap> l2g
      = std::make_shared<spmv::L2GMap>(comm, ncols_local, new_col_ghosts);
  spmv::Matrix b(B, l2g);
  return b;
}