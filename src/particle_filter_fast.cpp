#include "particle_filter_fast.h"
#include <GL/freeglut.h>
#include <tf/tf.h>
#include <tf_conversions/tf_eigen.h>

CPointcloudClassifier::CPointcloudClassifier(int cudaDevice,
			float normal_vectors_search_radius,
			float curvature_threshold,
			float ground_Z_coordinate_threshold,
			int number_of_points_needed_for_plane_threshold,
			int max_number_considered_in_INNER_bucket,
			int max_number_considered_in_OUTER_bucket)
	{
		this->cudaDevice = cudaDevice;
		this->normal_vectors_search_radius = normal_vectors_search_radius;
		this->curvature_threshold = curvature_threshold;
		this->ground_Z_coordinate_threshold = ground_Z_coordinate_threshold;
		this->number_of_points_needed_for_plane_threshold = number_of_points_needed_for_plane_threshold;
		this->max_number_considered_in_INNER_bucket = max_number_considered_in_INNER_bucket;
		this->max_number_considered_in_OUTER_bucket = max_number_considered_in_OUTER_bucket;

	}

void CPointcloudClassifier::classify (pcl::PointCloud<pcl::PointXYZ>in, pcl::PointCloud<Semantic::PointXYZL>&out)
{
//	float normal_vectors_search_radius = 1.0f;
//	float curvature_threshold = 0;
//	float ground_Z_coordinate_threshold = 1;
//	int number_of_points_needed_for_plane_threshold =1;
//	int max_number_considered_in_INNER_bucket = 100;
//	int max_number_considered_in_OUTER_bucket = 100;

	CCudaWrapper ccWraper;

	ccWraper.downsampling(in,0.2);

	 pcl::PointCloud<Semantic::PointXYZNL> data;
	 data.resize(in.size());
	 for (int i =0; i< in.size(); i++)
	 {
		 data[i].x = in[i].x;
		 data[i].y = in[i].y;
		 data[i].z = in[i].z;
	 }
	 ccWraper.classify(
			data,
			data.size(),
			normal_vectors_search_radius,
			curvature_threshold,
			ground_Z_coordinate_threshold,
			number_of_points_needed_for_plane_threshold,
			max_number_considered_in_INNER_bucket,
			max_number_considered_in_OUTER_bucket );

	 out.resize(data.size());
	 for (int i =0; i< data.size(); i++)
	 {
		 out[i].x = data[i].x;
		 out[i].y = data[i].y;
		 out[i].z = data[i].z;
		 out[i].label = data[i].label;

	 }
}


CParticleFilterFast::CParticleFilterFast()
{
	this->motion_model_max_angle = 10.0f;
	this->motion_model_max_translation = 2.0f;
	this->max_particle_size = 1000;

	this->cudaDevice = 0;
	this->d_first_point_cloud = NULL;
	this->number_of_points_first_point_cloud = 0;
	this->d_second_point_cloud = NULL;
	this->d_second_point_cloudT = NULL;
	this->d_nearest_neighbour_indexes = NULL;
	this->number_of_points_second_point_cloud = 0;
	this->rgd_res = 1.0f;
	this->bounding_box_extension = this->rgd_res;

	this->d_m = NULL;

	this->max_number_considered_in_INNER_bucket = 5;
	this->max_number_considered_in_OUTER_bucket = 5;

	this->overlap_threshold = 0.2f;
	this->propability_threshold = 0.01;

	this->d_hashTable_2D = NULL;
	this->d_buckets_2D = NULL;

	this->d_ground_points_from_map = NULL;
	this->number_of_points_ground_points_from_map = 0;

	this->d_rgd = NULL;
	this->rgd_2D_res = 5.0f;
	solutionOffset=Eigen::Affine3f::Identity();
}

CParticleFilterFast::~CParticleFilterFast()
{
	cudaFree(this->d_first_point_cloud); this->d_first_point_cloud = NULL;
	cudaFree(this->d_second_point_cloud); this->d_second_point_cloud = NULL;
	cudaFree(this->d_second_point_cloudT); this->d_second_point_cloudT = NULL;
	cudaFree(this->d_nearest_neighbour_indexes); this->d_nearest_neighbour_indexes = NULL;

	cudaFree(this->d_m); this->d_m = NULL;

	cudaFree(this->d_hashTable_2D);
	cudaFree(this->d_buckets_2D);

	cudaFree(this->d_rgd);

	std::cout << "CParticleFilter::~CParticleFilter(): DONE" << std::endl;
}

bool CParticleFilterFast::init(int cuda_device,
		float _motion_model_max_angle,
		float _motion_model_max_translation,
		float _max_particle_size,
		float _max_particle_size_kidnapped_robot,
		float _distance_above_Z,
		float _rgd_resolution,
		int _max_number_considered_in_INNER_bucket,
		int _max_number_considered_in_OUTER_bucket,
		float _overlap_threshold,
		float _propability_threshold,
		float _rgd_2D_res)
{
	if(!setCUDADevice(cuda_device))return false;

	this->motion_model_max_angle = _motion_model_max_angle;
	this->motion_model_max_translation = _motion_model_max_translation;
	this->max_particle_size = _max_particle_size;
	this->max_particle_size_kidnapped_robot = _max_particle_size_kidnapped_robot;
	this->distance_above_Z = _distance_above_Z;
	this->rgd_res = _rgd_resolution;
	this->max_number_considered_in_INNER_bucket = _max_number_considered_in_INNER_bucket;
	this->max_number_considered_in_OUTER_bucket = _max_number_considered_in_OUTER_bucket;
	this->overlap_threshold = _overlap_threshold;
	this->propability_threshold = _propability_threshold;

	this->rgd_2D_res = _rgd_2D_res;

	if(this->d_m != NULL)
	{
		cudaFree(this->d_m);
		this->d_m = NULL;
	}

	if(cudaMalloc((void**)&this->d_m, 16*sizeof(float) ) != ::cudaSuccess)
	{
		return false;
	}
return true;
}

bool CParticleFilterFast::prediction(Eigen::Affine3f odometryIncrement)
{
	clock_t begin_time;
	begin_time = clock();

	std::vector<float> vangle;
	std::vector<float> vtrans;

	for(size_t i = 0; i < vparticles.size(); i++)
	{
		vangle.push_back(((float(rand()%100000))/100000.0f * motion_model_max_angle * 2.0f - motion_model_max_angle) * M_PI/180.0);
		vtrans.push_back((float(rand()%100000))/100000.0f * motion_model_max_translation * 2.0f - motion_model_max_translation);
	}

	float *vmatrix = new float[vparticles.size() * 16];

	for(size_t i = 0; i < vparticles.size(); i++)
	{
		Eigen::Affine3f matrix = vparticles[i].v_particle_states[vparticles[i].v_particle_states.size()-1].matrix;

		vmatrix[16 * i + 0] = matrix.matrix()(0,0);
		vmatrix[16 * i + 1] = matrix.matrix()(1,0);
		vmatrix[16 * i + 2] = matrix.matrix()(2,0);
		vmatrix[16 * i + 3] = matrix.matrix()(3,0);

		vmatrix[16 * i + 4] = matrix.matrix()(0,1);
		vmatrix[16 * i + 5] = matrix.matrix()(1,1);
		vmatrix[16 * i + 6] = matrix.matrix()(2,1);
		vmatrix[16 * i + 7] = matrix.matrix()(3,1);

		vmatrix[16 * i + 8] = matrix.matrix()(0,2);
		vmatrix[16 * i + 9] = matrix.matrix()(1,2);
		vmatrix[16 * i + 10] = matrix.matrix()(2,2);
		vmatrix[16 * i + 11] = matrix.matrix()(3,2);

		vmatrix[16 * i + 12] = matrix.matrix()(0,3);
		vmatrix[16 * i + 13] = matrix.matrix()(1,3);
		vmatrix[16 * i + 14] = matrix.matrix()(2,3);
		vmatrix[16 * i + 15] = matrix.matrix()(3,3);
	}

	float *d_vangle;
	float *d_vtrans;
	float *d_vmatrix;
	float *d_odometryIncrement;

	if(cudaMalloc((void**)&d_vangle, vparticles.size()*sizeof(float)) != ::cudaSuccess)return false;
	if(cudaMalloc((void**)&d_vtrans, vparticles.size()*sizeof(float)) != ::cudaSuccess)return false;
	if(cudaMalloc((void**)&d_vmatrix, vparticles.size()*sizeof(float)*16) != ::cudaSuccess)return false;
	if(cudaMalloc((void**)&d_odometryIncrement, sizeof(float)*16) != ::cudaSuccess)return false;

	if(cudaMemcpy(d_vangle, vangle.data(), vparticles.size()*sizeof(float), cudaMemcpyHostToDevice) != ::cudaSuccess)return false;
	if(cudaMemcpy(d_vtrans, vtrans.data(), vparticles.size()*sizeof(float), cudaMemcpyHostToDevice) != ::cudaSuccess)return false;
	if(cudaMemcpy(d_vmatrix, vmatrix, vparticles.size()*sizeof(float) * 16, cudaMemcpyHostToDevice) != ::cudaSuccess)return false;
	if(cudaMemcpy(d_odometryIncrement, odometryIncrement.data(), sizeof(float) * 16, cudaMemcpyHostToDevice) != ::cudaSuccess)return false;

	if(cudaParticleFilterPrediction(this->number_of_threads,
			d_vangle,
			d_vtrans,
			distance_above_Z,
			d_vmatrix,
			vparticles.size(),
			d_odometryIncrement,
			this->d_hashTable_2D,
			this->d_buckets_2D,
			this->rgd_params_2D,
			this->rgd_2D_res,
			this->max_number_considered_in_INNER_bucket,
			this->max_number_considered_in_OUTER_bucket,
			this->d_ground_points_from_map,
			this->number_of_points_ground_points_from_map
			) != ::cudaSuccess)return false;

	if(cudaMemcpy(vmatrix, d_vmatrix, vparticles.size()*sizeof(float) * 16, cudaMemcpyDeviceToHost) != ::cudaSuccess)return false;

	if(cudaFree(d_vangle) != ::cudaSuccess)return false;
	if(cudaFree(d_vtrans) != ::cudaSuccess)return false;
	if(cudaFree(d_vmatrix) != ::cudaSuccess)return false;
	if(cudaFree(d_odometryIncrement) != ::cudaSuccess)return false;

	for(size_t i = 0; i < vparticles.size(); i++)
	{
		Eigen::Affine3f matrix;

		matrix.matrix()(0,0) = vmatrix[16 * i + 0];
		matrix.matrix()(1,0) = vmatrix[16 * i + 1];
		matrix.matrix()(2,0) = vmatrix[16 * i + 2];
		matrix.matrix()(3,0) = vmatrix[16 * i + 3];

		matrix.matrix()(0,1) = vmatrix[16 * i + 4];
		matrix.matrix()(1,1) = vmatrix[16 * i + 5];
		matrix.matrix()(2,1) = vmatrix[16 * i + 6];
		matrix.matrix()(3,1) = vmatrix[16 * i + 7];

		matrix.matrix()(0,2) = vmatrix[16 * i + 8];
		matrix.matrix()(1,2) = vmatrix[16 * i + 9];
		matrix.matrix()(2,2) = vmatrix[16 * i + 10];
		matrix.matrix()(3,2) = vmatrix[16 * i + 11];

		matrix.matrix()(0,3) = vmatrix[16 * i + 12];
		matrix.matrix()(1,3) = vmatrix[16 * i + 13];
		matrix.matrix()(2,3) = vmatrix[16 * i + 14];
		matrix.matrix()(3,3) = vmatrix[16 * i + 15];

		particle_state_t p;
		p.overlap = 0.0f;
		p.matrix = matrix;
		vparticles[i].v_particle_states.push_back(p);
	}

	free(vmatrix);

	double computation_time = (double)( clock () - begin_time ) /  CLOCKS_PER_SEC;
	std::cout << "computation_time_prediction: " << computation_time << std::endl;

	return true;
}

bool compareParticle(const CParticleFilterFast::particle_t& a, const CParticleFilterFast::particle_t& b)
{
	return a.nW > b.nW;
}

bool CParticleFilterFast::update()
{
	double computation_time_transformCurrentScan = 0.0;
	double computation_time_computeNN = 0.0;


	unsigned int *d_nn = NULL;
	cudaMalloc((void**)&d_nn, this->number_of_points_second_point_cloud*sizeof(unsigned int));

	for(size_t i = 0; i < this->vparticles.size(); i++)
	{
		CParticleFilterFast::particle_state_t particle_state;
		particle_state.matrix = this->vparticles[i].v_particle_states[this->vparticles[i].v_particle_states.size()-1].matrix;

		clock_t begin_time;
		begin_time = clock();
		if(!this->transformCurrentScan(particle_state.matrix))
		{
			std::cout << "problem with cuda_nn.transformScan(matrix) return false" << std::endl;
			return false;
		}

		computation_time_transformCurrentScan+=(double)( clock () - begin_time ) /  CLOCKS_PER_SEC;

		begin_time = clock();

		if(cudaComputeOverlap(
			this->number_of_threads,
			this->d_second_point_cloudT,
			this->number_of_points_second_point_cloud,
			this->d_rgd,
			this->rgd_params,
			d_nn,
			particle_state.overlap) != ::cudaSuccess)return false;
		particle_state.overlap =  particle_state.overlap * particle_state.overlap;
		computation_time_computeNN += (double)( clock () - begin_time ) /  CLOCKS_PER_SEC;

		if(particle_state.overlap < this->overlap_threshold)
		{
			this->vparticles[i].isOverlapOK = false;
		}else
		{
			this->vparticles[i].isOverlapOK = true;
		}

		this->vparticles[i].v_particle_states[this->vparticles[i].v_particle_states.size()-1].overlap = particle_state.overlap;
	}

	cudaFree(d_nn);

	std::cout << "computation_time_transformCurrentScan: " << computation_time_transformCurrentScan << std::endl;
	std::cout << "computation_time_computeNN: " << computation_time_computeNN << std::endl;

	if(vparticles.size() == 0)
	{
		genParticlesKidnappedRobot();
		return false;
	}

	std::vector<particle_t> vparticlesTemp;

	for(size_t i = 0; i < vparticles.size(); i++)
	{
		if(vparticles[i].isOverlapOK)
		{
			particle_t p = vparticles[i];
			vparticlesTemp.push_back(p);
		}
	}

	if(vparticlesTemp.size() == 0)
	{
		genParticlesKidnappedRobot();
		return false;
	}else
	{
		vparticles = vparticlesTemp;
	}

	for(size_t i = 0; i < vparticles.size(); i++)
	{
		vparticles[i].W += std::log(vparticles[i].v_particle_states[vparticles[i].v_particle_states.size()-1].overlap);
	}

	float lmax = vparticles[0].W;
	for(size_t i = 1; i < vparticles.size(); i++)
	{
		if(vparticles[i].W > lmax)lmax = vparticles[i].W;
	}

	float gain = 1.0f;
	float sum = 0.0f;
	for(size_t i = 0; i < vparticles.size(); i++)
	{
		vparticles[i].nW = std::exp((vparticles[i].W - lmax) * gain);
		sum += vparticles[i].nW;
	}

	std::sort(vparticles.begin(), vparticles.end(), compareParticle);

	vparticlesTemp.clear();

	for(size_t i = 0; i < vparticles.size(); i++)
	{
		if(vparticles[i].nW > this->propability_threshold)
		{
			particle_t p = vparticles[i];
			vparticlesTemp.push_back(p);
		}
	}

	int s = vparticlesTemp.size();

	if(s == 0)
	{
		genParticlesKidnappedRobot();
		return false;
	}

	if(s > max_particle_size) s = max_particle_size;

	for(size_t i = 0 ; i < (max_particle_size - s); i++)
	{
		vparticlesTemp.push_back(vparticles[0]);
	}

	vparticles = vparticlesTemp;

	std::cout << "vparticles.size: " << vparticles.size() << " replicated: " << max_particle_size - s << std::endl;

return true;
}

bool CParticleFilterFast::setCUDADevice(int _cudaDeveice)
{
	this->cudaDevice = _cudaDeveice;
	cudaError_t err = ::cudaSuccess;
		err = cudaSetDevice(this->cudaDevice);
			if(err != ::cudaSuccess)return false;

	cudaDeviceProp prop;
	cudaGetDeviceProperties(&prop, this->cudaDevice);

	int threads = 0;
	if(prop.major == 2)
	{
		this->number_of_threads = prop.maxThreadsPerBlock/2;
	}else if(prop.major > 2)
	{
		this->number_of_threads = prop.maxThreadsPerBlock;
	}else
	{
		return false;
	}
	return true;
}

bool CParticleFilterFast::setGroundPointsFromMap(pcl::PointCloud<pcl::PointXYZ> pc)
{
	cudaError_t errCUDA = ::cudaSuccess;
	this->ground_points_from_map = pc;

	if(this->d_ground_points_from_map != NULL)
	{
		if(cudaFree(this->d_ground_points_from_map) != ::cudaSuccess)return false;
		this->d_ground_points_from_map = NULL;
		this->number_of_points_ground_points_from_map = 0;
	}

	this->number_of_points_ground_points_from_map = this->ground_points_from_map.size();

	errCUDA  = cudaMalloc((void**)&this->d_ground_points_from_map, this->number_of_points_ground_points_from_map*sizeof(pcl::PointXYZ) );
	if(errCUDA != ::cudaSuccess){return false;}

	errCUDA = cudaMemcpy(this->d_ground_points_from_map, this->ground_points_from_map.points.data(), this->number_of_points_ground_points_from_map*sizeof(pcl::PointXYZ),cudaMemcpyHostToDevice);
	if(errCUDA != ::cudaSuccess){return false;}

	return true;
}

bool CParticleFilterFast::computeRGD()
{
	cudaError_t errCUDA = ::cudaSuccess;

	errCUDA = cudaCalculateGridParams(this->d_first_point_cloud, number_of_points_first_point_cloud,
				this->rgd_res, this->rgd_res, this->rgd_res, this->bounding_box_extension, rgd_params);
			if(errCUDA != ::cudaSuccess)return false;

	std::cout << "regular grid parameters:" << std::endl;
	std::cout << "bounding_box_min_X: " << this->rgd_params.bounding_box_min_X << std::endl;
	std::cout << "bounding_box_min_Y: " << this->rgd_params.bounding_box_min_Y << std::endl;
	std::cout << "bounding_box_min_Z: " << this->rgd_params.bounding_box_min_Z << std::endl;
	std::cout << "bounding_box_max_X: " << this->rgd_params.bounding_box_max_X << std::endl;
	std::cout << "bounding_box_max_Y: " << this->rgd_params.bounding_box_max_Y << std::endl;
	std::cout << "bounding_box_max_Z: " << this->rgd_params.bounding_box_max_Z << std::endl;
	std::cout << "number_of_buckets_X: " << this->rgd_params.number_of_buckets_X << std::endl;
	std::cout << "number_of_buckets_Y: " << this->rgd_params.number_of_buckets_Y << std::endl;
	std::cout << "number_of_buckets_Z: " << this->rgd_params.number_of_buckets_Z << std::endl;
	std::cout << "resolution_X: " << this->rgd_params.resolution_X << std::endl;
	std::cout << "resolution_Y: " << this->rgd_params.resolution_Y << std::endl;
	std::cout << "resolution_Z: " << this->rgd_params.resolution_Z << std::endl;
	std::cout << "number_of_buckets: " << this->rgd_params.number_of_buckets << std::endl;

	errCUDA = cudaCalculateGridParams2D(this->d_ground_points_from_map, this->number_of_points_ground_points_from_map,
			this->rgd_2D_res, this->rgd_2D_res, this->bounding_box_extension, this->rgd_params_2D);
			if(errCUDA != ::cudaSuccess)return false;


	std::cout << "----------------------------------------" << std::endl;
	std::cout << "regular grid 2D parameters:" << std::endl;
	std::cout << "bounding_box_min_X: " << this->rgd_params_2D.bounding_box_min_X << std::endl;
	std::cout << "bounding_box_min_Y: " << this->rgd_params_2D.bounding_box_min_Y << std::endl;
	std::cout << "bounding_box_max_X: " << this->rgd_params_2D.bounding_box_max_X << std::endl;
	std::cout << "bounding_box_max_Y: " << this->rgd_params_2D.bounding_box_max_Y << std::endl;
	std::cout << "number_of_buckets_X: " << this->rgd_params_2D.number_of_buckets_X << std::endl;
	std::cout << "number_of_buckets_Y: " << this->rgd_params_2D.number_of_buckets_Y << std::endl;
	std::cout << "resolution_X: " << this->rgd_params_2D.resolution_X << std::endl;
	std::cout << "resolution_Y: " << this->rgd_params_2D.resolution_Y << std::endl;
	std::cout << "number_of_buckets: " << this->rgd_params_2D.number_of_buckets << std::endl;

	errCUDA = cudaMalloc((void**)&d_hashTable_2D, this->number_of_points_ground_points_from_map*sizeof(hashElement));
			if(errCUDA != ::cudaSuccess)return false;

	errCUDA = cudaMalloc((void**)&d_buckets_2D, rgd_params_2D.number_of_buckets*sizeof(bucket));
			if(errCUDA != ::cudaSuccess)return false;

	errCUDA = cudaCalculateGrid2D(this->number_of_threads, this->d_ground_points_from_map, this->d_buckets_2D, this->d_hashTable_2D, this->number_of_points_ground_points_from_map, this->rgd_params_2D);
			if(errCUDA != ::cudaSuccess)return false;

	errCUDA = cudaMalloc((void**)&this->d_rgd, this->rgd_params.number_of_buckets *sizeof(char) );
			if(errCUDA != ::cudaSuccess)return false;

	char *h_rgd = (char *)malloc(this->rgd_params.number_of_buckets * sizeof(char));
	memset(h_rgd, -1, this->rgd_params.number_of_buckets * sizeof(char));
	cudaMemcpy(this->d_rgd, h_rgd, this->rgd_params.number_of_buckets * sizeof(char) ,cudaMemcpyHostToDevice);
	free(h_rgd);

	if(cudaInsertPointCloudToRGD(this->number_of_threads,
			this->d_first_point_cloud,
			this->number_of_points_first_point_cloud,
			this->d_rgd,
			this->rgd_params) != ::cudaSuccess)return false;
return true;
}

bool CParticleFilterFast::copyReferenceModelToGPU(pcl::PointCloud<Semantic::PointXYZL> &reference_model)
{
	cudaError_t errCUDA = cudaSetDevice(this->cudaDevice);
	if(errCUDA != ::cudaSuccess)
	{
		std::cout << "Check if cudaDevice=" << cudaDevice << " is in the system" << std::endl;
		return false;
	}

	if(this->d_first_point_cloud != NULL)
	{
		cudaFree(this->d_first_point_cloud);
		this->d_first_point_cloud = NULL;
		this->number_of_points_first_point_cloud = 0;
	}

	this->number_of_points_first_point_cloud = reference_model.size();

	errCUDA  = cudaMalloc((void**)&this->d_first_point_cloud, this->number_of_points_first_point_cloud*sizeof(Semantic::PointXYZL) );
	if(errCUDA != ::cudaSuccess){cudaDeviceReset();return false;}

	errCUDA = cudaMemcpy(this->d_first_point_cloud, reference_model.points.data(), number_of_points_first_point_cloud*sizeof(Semantic::PointXYZL),cudaMemcpyHostToDevice);
	if(errCUDA != ::cudaSuccess){cudaDeviceReset();return false;}

	return true;
}

bool CParticleFilterFast::copyCurrentScanToGPU(pcl::PointCloud<Semantic::PointXYZL> &current_scan)
{
	cudaError_t errCUDA = ::cudaSuccess;

	if(this->d_second_point_cloud != NULL)
	{
		cudaFree(this->d_second_point_cloud);
		this->d_second_point_cloud = NULL;
		this->number_of_points_second_point_cloud = 0;
	}

	if(this->d_second_point_cloudT != NULL)
	{
		cudaFree(this->d_second_point_cloudT);
		this->d_second_point_cloudT = NULL;
	}

	if(this->d_nearest_neighbour_indexes != NULL)
	{
		cudaFree(this->d_nearest_neighbour_indexes);
		this->d_nearest_neighbour_indexes = NULL;
	}

	this->number_of_points_second_point_cloud = current_scan.size();

	errCUDA  = cudaMalloc((void**)&this->d_second_point_cloud, this->number_of_points_second_point_cloud*sizeof(Semantic::PointXYZL) );
	if(errCUDA != ::cudaSuccess){cudaDeviceReset();return false;}

	errCUDA  = cudaMalloc((void**)&this->d_second_point_cloudT, this->number_of_points_second_point_cloud*sizeof(Semantic::PointXYZL) );
	if(errCUDA != ::cudaSuccess){cudaDeviceReset();return false;}

	errCUDA  = cudaMalloc((void**)&this->d_nearest_neighbour_indexes, this->number_of_points_second_point_cloud*sizeof(int) );
	if(errCUDA != ::cudaSuccess){cudaDeviceReset();return false;}

	errCUDA = cudaMemcpy(this->d_second_point_cloud, current_scan.points.data(), number_of_points_second_point_cloud*sizeof(Semantic::PointXYZL),cudaMemcpyHostToDevice);
	if(errCUDA != ::cudaSuccess){cudaDeviceReset();return false;}

	this->h_second_point_cloud = current_scan;
	this->h_nearest_neighbour_indexes.resize(this->h_second_point_cloud.size());
	return true;
}

bool CParticleFilterFast::transformCurrentScan(Eigen::Affine3f matrix)
{
	if(cudaMemcpy(this->d_m, matrix.data(), 16*sizeof(float),cudaMemcpyHostToDevice)!= ::cudaSuccess)
	{
		return false;
	}

	if(cudaTransformPointCloud(this->number_of_threads,
			this->d_second_point_cloud,
			this->d_second_point_cloudT,
			this->number_of_points_second_point_cloud,
			d_m) != ::cudaSuccess)
	{
		return false;
	}

	return true;
}

void CParticleFilterFast::genParticlesKidnappedRobot()
{
	this->vparticles.clear();
	if (ground_points_from_map.size() == 0 ) return;
	for(int i = 0 ; i < max_particle_size_kidnapped_robot/18; i++)
	{
		particle_state_t p;
		p.matrix = Eigen::Affine3f::Identity();
		pcl::PointXYZ point;
		int index = rand()%(ground_points_from_map.size()-1);
		if (index < 0)index = 0;
		point = ground_points_from_map[index];
		point.z += distance_above_Z;

		p.matrix(0,3) = point.x;
		p.matrix(1,3) = point.y;
		p.matrix(2,3) = point.z;

		p.overlap = 0.0f;

		particle_t pp;
		pp.v_particle_states.push_back(p);
		pp.W = 0.0f;
		pp.nW = 0.0f;
		pp.isOverlapOK = false;
		vparticles.push_back(pp);
	}

	std::vector<particle_t> vparticlesTemp;

	for(size_t i = 0 ; i < vparticles.size(); i++)
	{
		vparticlesTemp.push_back(vparticles[i]);

		for(float angle = 0.0; angle < 360.0; angle += 20.0)
		{
			float anglaRad = angle*M_PI/180.0;

			Eigen::Affine3f m;
					m = Eigen::AngleAxisf(0.0f, Eigen::Vector3f::UnitX())
						  * Eigen::AngleAxisf(0.0f, Eigen::Vector3f::UnitY())
						  * Eigen::AngleAxisf(anglaRad, Eigen::Vector3f::UnitZ());

			particle_t particle = vparticles[i];
			particle.v_particle_states[0].matrix = particle.v_particle_states[0].matrix * m;

			vparticlesTemp.push_back(particle);
		}
	}
	vparticles = vparticlesTemp;
}

bool CParticleFilterFast::findClosestParticle(Eigen::Vector3f _reference_pose,
		Eigen::Vector3f &out_particle)
{
	double dist = 1000000.0;
	out_particle = Eigen::Vector3f(0.0f,0.0f,0.0f);

	for(size_t i = 0; i < ground_points_from_map.size(); i++)
	{
		Eigen::Vector3f p(ground_points_from_map[i].x, ground_points_from_map[i].y, ground_points_from_map[i].z);
		double _dist =  (_reference_pose.x() - p.x()) * (_reference_pose.x() - p.x()) +
				(_reference_pose.y() - p.y()) * (_reference_pose.y() - p.y());

		if(_dist < dist)
		{
			dist = _dist;
			out_particle = p;
		}
	}
	return true;
}

Eigen::Affine3f CParticleFilterFast::getWinningParticle()
{
	Eigen::Affine3f out_matrix;
	if(vparticles.size() == 0)return Eigen::Affine3f::Identity();
	out_matrix = solutionOffset * vparticles[0].v_particle_states[vparticles[0].v_particle_states.size()-1].matrix;
return out_matrix;
}


void CParticleFilterFast::render()
{
	glPushAttrib (GL_ALL_ATTRIB_BITS);

	glPointSize(5.0);
	glColor3f(0.0f, 0.0f, 1.0f);
	glBegin(GL_POINTS);
	if(vparticles.size() > 0)
	{
		for(size_t i = 0 ; i < vparticles.size(); i++)
		{
			if(vparticles[i].v_particle_states.size()>0)
			{
				float x = vparticles[i].v_particle_states[vparticles[i].v_particle_states.size()-1].matrix(0,3);
				float y = vparticles[i].v_particle_states[vparticles[i].v_particle_states.size()-1].matrix(1,3);
				float z = vparticles[i].v_particle_states[vparticles[i].v_particle_states.size()-1].matrix(2,3);

				glVertex3f(x,y,z);
			}
		}
	}
	glEnd();

	glPopAttrib();
}

nav_msgs::Odometry CParticleFilterFast::getOdom()
{

	double sumX =0;
	double sumY =0;
	double sumZ =0;
	double sumYaw =0;



	double sumWeights = 0;

	double num = 0;
	for(size_t i = 0 ; i < vparticles.size(); i++)
	{
		if(vparticles[i].v_particle_states.size()>0)
		{
			particle_state_t state = vparticles[i].v_particle_states[vparticles[i].v_particle_states.size()-1];
			Eigen::Affine3f matrixOffseted = solutionOffset * state.matrix;
			double weight = vparticles[i].W;
			sumX +=weight * matrixOffseted(0,3);
			sumY +=weight * matrixOffseted(1,3);
			sumZ +=weight * matrixOffseted(2,3);

			Eigen::Vector3f rpy = matrixOffseted.rotation().eulerAngles(0, 1, 2);
			sumYaw += weight * rpy(2);
			sumWeights += weight;
		}
	}


	double uX   = sumX / (sumWeights);
	double uY   = sumY / (sumWeights);
	double uZ   = sumZ / (sumWeights);
	double uYaw = sumYaw / (sumWeights);

	double qqX =0;
	double qqY =0;
	double qqZ =0;

	double qqYaw =0;
	sumWeights = 0;
	for(size_t i = 0 ; i < vparticles.size(); i++)
	{
		if(vparticles[i].v_particle_states.size()>0)
		{
			particle_state_t state = vparticles[i].v_particle_states[vparticles[i].v_particle_states.size()-1];
			Eigen::Affine3f matrixOffseted = solutionOffset * state.matrix;
			double weight = fabs(vparticles[i].W);
			qqX +=  weight * (matrixOffseted(0,3) - uX)*(matrixOffseted(0,3) - uX);
			qqY +=  weight * (matrixOffseted(1,3) - uY)*(matrixOffseted(1,3) - uY);
			qqZ +=  weight * (matrixOffseted(2,3) - uZ)*(matrixOffseted(2,3) - uZ);


			Eigen::Vector3f rpy = matrixOffseted.rotation().eulerAngles(0, 1, 2);
			qqYaw += weight * (uYaw - rpy(2)) * (uYaw - rpy(2));
			sumWeights += weight;
			if (weight!=0)
			{
				num = num + 1.0;
			}

		}
	}




	nav_msgs::Odometry odoMsg;



	odoMsg.pose.covariance[0]  = 10* qqX   / ( (num - 1) * sumWeights  / num ) ;
	odoMsg.pose.covariance[7]  = 10*qqY   / ( (num - 1) * sumWeights  / num );
	odoMsg.pose.covariance[35] = 10*qqYaw / ( (num - 1) * sumWeights  / num );

	Eigen::Quaterniond eigenQuatd(
			Eigen::AngleAxisd(0.0f, Eigen::Vector3d::UnitX())*
	  	  	Eigen::AngleAxisd(0.0f, Eigen::Vector3d::UnitY())*
			Eigen::AngleAxisd(uYaw, Eigen::Vector3d::UnitZ())
	);


	odoMsg.pose.pose.position.x = uX;
	odoMsg.pose.pose.position.y = uY;
	odoMsg.pose.pose.position.z = uZ;

	odoMsg.pose.pose.orientation.w = eigenQuatd.w();
	odoMsg.pose.pose.orientation.x = eigenQuatd.x();
	odoMsg.pose.pose.orientation.y = eigenQuatd.y();
	odoMsg.pose.pose.orientation.z = eigenQuatd.z();



	odoMsg.child_frame_id="";
	odoMsg.header.frame_id = "map";
	odoMsg.header.stamp = ros::Time::now();


	return odoMsg;
}
geometry_msgs::PoseArray CParticleFilterFast::getPoseArray()
{

	geometry_msgs::PoseArray poseArray;
	poseArray.header.stamp    = ros::Time::now();
	poseArray.header.frame_id = "map";


	if(vparticles.size() > 0)
	{
		for(size_t i = 0 ; i < vparticles.size(); i++)
		{
			if(vparticles[i].v_particle_states.size()>0)
			{
				geometry_msgs::Pose pose;
				Eigen::Affine3f state = solutionOffset * vparticles[i].v_particle_states[vparticles[i].v_particle_states.size()-1].matrix;
				pose.position.x = state(0,3);
				pose.position.y = state(1,3);
				pose.position.z = state(2,3);
				Eigen::Quaternionf qf( state.rotation());
				pose.orientation.w = qf.w();
				pose.orientation.x = qf.x();
				pose.orientation.y = qf.y();
				pose.orientation.z = qf.z();
				poseArray.poses.push_back(pose);

			}
		}
	}
	return poseArray;
}
void CParticleFilterFast::setPose (geometry_msgs::PoseWithCovarianceStamped pose)
{
	this->vparticles.clear();

	double meanX = pose.pose.pose.position.x;
	double meanY = pose.pose.pose.position.y;
	double meanZ = pose.pose.pose.position.z;
	double diffX = pose.pose.covariance.elems[0];
	double diffY = pose.pose.covariance.elems[7];
	double diffYaw = pose.pose.covariance.elems[35];
	tf::Quaternion qaternion;
	tf::quaternionMsgToTF(pose.pose.pose.orientation,qaternion);
	double t1,t2,meanYaw;
	tf::Matrix3x3(qaternion).getEulerYPR(meanYaw,t1,t2);

	std::vector<particle_t> vparticlesTemp;

	for(int i = 0 ; i < max_particle_size_kidnapped_robot; i++)
	{
		particle_state_t p;
		p.matrix = Eigen::Affine3f::Identity();
		pcl::PointXYZ point;


		p.matrix(0,3) = meanX+randFloat()*diffX;
		p.matrix(1,3) = meanY+randFloat()*diffX;;
		p.matrix(2,3) = distance_above_Z;

		p.overlap = 0.0f;

		particle_t pp;
		pp.v_particle_states.push_back(p);
		pp.W = 0.0f;
		pp.nW = 0.0f;
		pp.isOverlapOK = false;
		vparticles.push_back(pp);

		Eigen::Affine3f m;
				m = Eigen::AngleAxisf(0.0f, Eigen::Vector3f::UnitX())
					  * Eigen::AngleAxisf(0.0f, Eigen::Vector3f::UnitY())
					  * Eigen::AngleAxisf(meanYaw+diffYaw*randFloat(), Eigen::Vector3f::UnitZ());

		particle_t particle = vparticles[i];
		particle.v_particle_states[0].matrix = solutionOffset.inverse() * particle.v_particle_states[0].matrix * m;

		vparticlesTemp.push_back(particle);
	}



	vparticles = vparticlesTemp;
}
