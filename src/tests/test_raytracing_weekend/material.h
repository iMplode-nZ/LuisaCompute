#pragma once

#include "rtweekend.h"

class hit_record;

class material {
public:
    virtual Bool scatter(
        const ray &r_in, const hit_record &rec, Float3 &attenuation, ray &scattered, UInt &seed) const = 0;
};

class lambertian : public material {
public:
    lambertian(const float3 &a) : albedo(a) {}

    virtual Bool scatter(
        const ray &r_in, const hit_record &rec, Float3 &attenuation, ray &scattered, UInt &seed) const override {
        Float3 scatter_direction = rec.normal + random_unit_vector(seed);

        // Catch degenerate scatter direction
        $if(near_zero(scatter_direction)) {
            scatter_direction = rec.normal;
        };

        scattered = ray(rec.p, scatter_direction);
        attenuation = albedo;
        return true;
    }

public:
    float3 albedo;
};

class metal : public material {
public:
    metal(const float3 &a, float f) : albedo(a), fuzz(f < 1 ? f : 1) {}

    virtual Bool scatter(
        const ray &r_in, const hit_record &rec, Float3 &attenuation, ray &scattered, UInt &seed) const override {
        Float3 reflected = ray_reflect(normalize(r_in.direction()), rec.normal);
        scattered = ray(rec.p, reflected + fuzz * random_in_unit_sphere(seed));
        attenuation = albedo;
        return (dot(scattered.direction(), rec.normal) > 0);
    }

public:
    float3 albedo;
    float fuzz;
};

class dielectric : public material {
public:
    dielectric(float index_of_refraction) : ir(index_of_refraction) {}

    virtual Bool scatter(
        const ray &r_in, const hit_record &rec, Float3 &attenuation, ray &scattered, UInt &seed) const override {
        attenuation = make_float3(1.0f, 1.0f, 1.0f);
        Float refraction_ratio = select(ir, 1.0f / ir, rec.front_face);

        Float3 unit_direction = normalize(r_in.direction());
        Float cos_theta = min(dot(-unit_direction, rec.normal), 1.0f);
        Float sin_theta = sqrt(1.0f - cos_theta * cos_theta);

        Bool cannot_refract = refraction_ratio * sin_theta > 1.0f;
        Float3 direction;

        $if(cannot_refract | reflectance(cos_theta, refraction_ratio) > frand(seed)) {
            direction = ray_reflect(unit_direction, rec.normal);
        }
        $else {
            direction = ray_refract(unit_direction, rec.normal, refraction_ratio);
        };

        scattered = ray(rec.p, direction);
        return true;
    }

public:
    float ir;// Index of Refraction

private:
    static Float reflectance(Float cosine, Float ref_idx) {
        // Use Schlick's approximation for reflectance.
        Float r0 = (1.0f - ref_idx) / (1.0f + ref_idx);
        r0 = r0 * r0;
        return r0 + (1.0f - r0) * pow((1.0f - cosine), 5.0f);
    }
};

