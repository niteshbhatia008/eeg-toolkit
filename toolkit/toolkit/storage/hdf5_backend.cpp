#include "backends.hpp"

#define DATA_RANK 2
#define ATTR_RANK 1
#define NUM_ATTR 2
#define ATTR_NAME "metadata" // we store fs and nsamples here, all datasets have the same attribute name
#define FS_IDX 0
#define DATA_LEN_IDX 1

// TODO(joshblum): why can't we use the base class method?
// TODO(joshblum): place data in different directory
string HDF5Backend::mrn_to_array_name(string mrn)
{
  return mrn + ".h5";
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

int HDF5Backend::get_data_len(string mrn)
{
  return get_array_metadata(mrn, DATA_LEN_IDX);
}

void HDF5Backend::load_array(string mrn)
{
  string array_name = mrn_to_array_name(mrn);
  H5File file(array_name, H5F_ACC_RDWR);
  DataSet dataset = file.openDataSet(array_name);
  put_cache(mrn, dataset);
}

void HDF5Backend::get_array_data(string mrn, int ch, int start_offset, int end_offset, frowvec& buf)
{
  DataSet dataset = get_cache(mrn);
  hsize_t offset[DATA_RANK], count[DATA_RANK], stride[DATA_RANK], block[DATA_RANK];

  offset[0] = start_offset; // read start_offset rows down
  offset[1] = CH_REVERSE_IDX[ch]; // read the correct column

  count[0]  = end_offset - start_offset; // nsamples to read
  count[1]  = 1; // only ever read one column

  // TODO(joshblum): use this for downsampling
  stride[0] = 1;
  stride[1] = 1;

  block[0] = 1;
  block[1] = 1;

  DataSpace memspace(DATA_RANK, count, NULL);
  DataSpace dataspace = dataset.getSpace();
  dataspace.selectHyperslab(H5S_SELECT_SET, count, offset, stride, block);

  dataset.read(buf.memptr(), PredType::NATIVE_FLOAT, memspace, dataspace);
}

void HDF5Backend::close_array(string mrn)
{
  DataSet dataset = get_cache(mrn);
  dataset.close();
  pop_cache(mrn);
}


void HDF5Backend::edf_to_array(string mrn)
{
  EDFBackend edf_backend;
  edf_backend.load_array(mrn);

  int nchannels = NCHANNELS;
  int nsamples = edf_backend.get_data_len(mrn);
  int fs = edf_backend.get_fs(mrn);

  cout << "Converting mrn: " << mrn << " with " << nsamples << " samples and fs=" << fs <<endl;

  fmat data = fmat(nchannels, nsamples);
  frowvec buf = frowvec(nsamples);

  // TODO(joshblum): chunk the reads
  for (int i = 0; i < nchannels; i++)
  {
    int ch = CHANNEL_ARRAY[i];
    buf = data.row(i);
    edf_backend.get_array_data(mrn, ch, 0, nsamples, buf);
    data.row(i) = buf;
  }

  cout << "Read data into memory" << endl;

  string array_name = mrn_to_array_name(mrn);

  H5File file(array_name, H5F_ACC_TRUNC);

  hsize_t data_dims[DATA_RANK];
  data_dims[0] = nsamples;
  data_dims[1] = nchannels;
  DataSpace dataspace(DATA_RANK, data_dims);
  DataSet dataset = file.createDataSet(array_name, PredType::NATIVE_FLOAT, dataspace);

  // Write signal data
  dataset.write(data.memptr(), PredType::NATIVE_FLOAT);

  cout << "Wrote data." << endl;

  int attr_data[NUM_ATTR];
  attr_data[FS_IDX] = fs;
  attr_data[DATA_LEN_IDX] = nsamples;
  hsize_t attr_dims[ATTR_RANK] = {NUM_ATTR};
  DataSpace attrspace = DataSpace(ATTR_RANK, attr_dims);

  Attribute attribute = dataset.createAttribute(ATTR_NAME, PredType::STD_I32BE, attrspace);

  // Write the attribute data.
  attribute.write(PredType::NATIVE_INT, attr_data);
}
