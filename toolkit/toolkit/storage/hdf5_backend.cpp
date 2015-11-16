#include "backends.hpp"

#include<string>
#include<armadillo>

using namespace std;
using namespace arma;

#define DATA_RANK 2
#define ATTR_RANK 1
#define NUM_ATTR 2
#define ATTR_NAME "metadata" // we store fs and nsamples here, all datasets have the same attribute name
#define FS_IDX 0
#define DATA_LEN_IDX 1

string HDF5Backend::mrn_to_array_name(string mrn)
{
  return AbstractStorageBackend::_mrn_to_array_name(mrn, "h5");
}

int HDF5Backend::get_array_metadata(string mrn, int metadata_idx)
{
  DataSet dataset = get_cache(mrn);
  Attribute attr = dataset.openAttribute(ATTR_NAME);
  int attr_data[NUM_ATTR];
  attr.read(PredType::NATIVE_INT, attr_data);
  return attr_data[metadata_idx];
}

int HDF5Backend::get_fs(string mrn)
{
  return get_array_metadata(mrn, FS_IDX);
}

int HDF5Backend::get_array_len(string mrn)
{
  return get_array_metadata(mrn, DATA_LEN_IDX);
}

void HDF5Backend::create_array(string mrn, ArrayMetadata* metadata)
{
  string array_name = mrn_to_array_name(mrn);
  H5File file(array_name, H5F_ACC_TRUNC);

  // Create the dataset of the correct dimensions
  hsize_t data_dims[DATA_RANK];
  data_dims[0] = metadata->nrows;
  data_dims[1] = metadata->ncols;
  DataSpace dataspace(DATA_RANK, data_dims);
  DataSet dataset = file.createDataSet(mrn, PredType::NATIVE_FLOAT, dataspace);

  // Write fs and data_len as attributes
  int attr_data[NUM_ATTR];
  attr_data[FS_IDX] = metadata->fs;
  attr_data[DATA_LEN_IDX] = metadata->nsamples;
  hsize_t attr_dims[ATTR_RANK] = {NUM_ATTR};
  DataSpace attrspace = DataSpace(ATTR_RANK, attr_dims);

  Attribute attribute = dataset.createAttribute(ATTR_NAME, PredType::STD_I32BE, attrspace);

  attribute.write(PredType::NATIVE_INT, attr_data);
}

void HDF5Backend::open_array(string mrn)
{
  if (!array_exists(mrn)) {
    cout << "Error array " << mrn_to_array_name(mrn) << " does not exist!" << endl;
    exit(1);
  }
  if (in_cache(mrn))
  {
    return;
  }
  string array_name = mrn_to_array_name(mrn);
  H5File file(array_name, H5F_ACC_RDWR);
  DataSet dataset = file.openDataSet(mrn);
  put_cache(mrn, dataset);
}

void HDF5Backend::read_array(string mrn, int ch, int start_offset, int end_offset, frowvec& buf)
{
  DataSet dataset = get_cache(mrn);
  hsize_t offset[DATA_RANK], count[DATA_RANK];

  offset[0] = start_offset; // start_offset rows down
  offset[1] = CH_REVERSE_IDX[ch]; // get the correct column

  count[0] = end_offset - start_offset;
  count[1] = buf.n_rows; // only ever get one column

  _read_array(mrn, offset, count, buf.memptr());
}

void HDF5Backend::read_array(string mrn, int start_offset, int end_offset, fmat& buf)
{
  hsize_t offset[DATA_RANK], count[DATA_RANK];
  offset[0] = start_offset; // start_offset rows down
  offset[1] = 0; //  we want all columns

  count[0] = end_offset - start_offset;
  count[1] = buf.n_rows;
  _read_array(mrn, offset, count, buf.memptr());
}

void HDF5Backend::_read_array(string mrn, hsize_t offset[], hsize_t count[], void* buf)
{
  DataSet dataset = get_cache(mrn);
  hsize_t stride[DATA_RANK], block[DATA_RANK];

  // TODO(joshblum): use this for downsampling
  stride[0] = 1;
  stride[1] = 1;

  block[0] = 1;
  block[1] = 1;

  DataSpace memspace(DATA_RANK, count, NULL);
  DataSpace dataspace = dataset.getSpace();
  dataspace.selectHyperslab(H5S_SELECT_SET, count, offset, stride, block);

  dataset.read(buf, PredType::NATIVE_FLOAT, memspace, dataspace);
}

void HDF5Backend::write_array(string mrn, int ch, int start_offset, int end_offset, fmat& buf)
{
  DataSet dataset = get_cache(mrn);
  hsize_t offset[DATA_RANK], count[DATA_RANK];

  offset[0] = start_offset; // start_offset rows down
  offset[1] = CH_REVERSE_IDX[ch]; // get the correct column

  count[0] = end_offset - start_offset;
  count[1] = buf.n_rows;

  DataSpace dataspace = dataset.getSpace();
  dataspace.selectHyperslab(H5S_SELECT_SET, count, offset);

  DataSpace memspace(DATA_RANK, count, NULL);
  dataset.write(buf.memptr(), PredType::NATIVE_FLOAT, memspace, dataspace);

}

void HDF5Backend::close_array(string mrn)
{
  if (in_cache(mrn)) {
    DataSet dataset = get_cache(mrn);
    dataset.close();
    pop_cache(mrn);
  }
}

void HDF5Backend::edf_to_array(string mrn)
{
  EDFBackend edf_backend;
  edf_backend.open_array(mrn);

  int nchannels = NCHANNELS;
  int nsamples = edf_backend.get_array_len(mrn);
  int fs = edf_backend.get_fs(mrn);

  cout << "Converting mrn: " << mrn << " with " << nsamples << " samples and fs=" << fs <<endl;
  ArrayMetadata metadata = ArrayMetadata(fs, nsamples, nsamples, nchannels);
  create_array(mrn, &metadata);
  open_array(mrn);

  int ch, start_offset, end_offset;
  for (int i = 0; i < nchannels; i++)
  {
    ch = CHANNEL_ARRAY[i];
    start_offset = 0;
    end_offset = min(nsamples, CHUNK_SIZE);
    frowvec chunk_buf = frowvec(end_offset);

    // read chunks from each signal and write them
    for (; end_offset <= nsamples; end_offset = min(end_offset + CHUNK_SIZE, nsamples))
    {
      if (end_offset - start_offset != CHUNK_SIZE) {
        chunk_buf.resize(end_offset - start_offset);
      }
      edf_backend.read_array(mrn, ch, start_offset, end_offset, chunk_buf);
      fmat chunk_mat = conv_to<fmat>::from(chunk_buf);
      write_array(mrn, ch, start_offset, end_offset, chunk_mat);

      start_offset = end_offset;
      // ensure we write the last part of the samples
      if (end_offset == nsamples)
      {
        break;
      }

      if (!(ch % 2 || (end_offset / CHUNK_SIZE) % 10)) // print for even channels every 10 chunks (40MB)
      {
        cout << "Wrote " << end_offset / CHUNK_SIZE << " chunks for ch: " << ch << endl;
      }
    }
  }

  cout << "Write complete" << endl;
}

